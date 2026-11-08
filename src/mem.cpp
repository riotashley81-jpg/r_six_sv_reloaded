#include "mem.h"

#include <windows.h>

#include <mutex>
#include <vector>

namespace mem {

u8* base = nullptr;
static std::vector<u8> committed;  // one byte per 4 KiB guest page
static std::mutex commit_lock;

void init() {
    base = (u8*)VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) fatal("cannot reserve 4 GiB guest address space");
    committed.assign(1u << 20, 0);
    LOGI("guest memory at host %p", base);
}

void commit(u32 addr, u32 size) {
    std::lock_guard lk(commit_lock);
    u32 first = addr >> 12, last = (u32)(((u64)addr + size + 0xFFF) >> 12);
    for (u32 p = first; p < last; p++) {
        if (committed[p]) continue;
        // commit runs of uncommitted pages at once
        u32 q = p;
        while (q < last && !committed[q]) committed[q++] = 1;
        if (!VirtualAlloc(base + ((u64)p << 12), (u64)(q - p) << 12, MEM_COMMIT, PAGE_READWRITE))
            fatal("commit failed at %08x", p << 12);
        p = q - 1;
    }
}

bool valid(u32 addr) { return committed[addr >> 12] != 0; }

// ---------------------------------------------------------------------------
// Heap: boundary-tag allocator with segregated free lists.
// Every block has a 16-byte header; payloads are 16-byte aligned.

struct Hdr {
    u32 prev_size;  // size of the physically previous block (0 for the first)
    u32 size;       // block size including header; bit 0 = in use
    u32 next_free;  // guest address of next free block in bin (free blocks only)
    u32 prev_free;
};
constexpr u32 HDR = 16, MIN_BLOCK = 32;
constexpr int SMALL_BINS = 65;  // exact sizes 0..1024 step 16
constexpr int NUM_BINS = SMALL_BINS + 24;

static std::mutex heap_lock;
static u32 bins[NUM_BINS];
static u32 heap_top = HEAP_BASE;     // first address past the last block
static u32 heap_commit = HEAP_BASE;  // committed up to here
static u32 last_block = 0;           // header of the physically last block

static Hdr& hdr(u32 b) { return ref<Hdr>(b); }
static u32 bsize(u32 b) { return hdr(b).size & ~1u; }
static bool used(u32 b) { return hdr(b).size & 1; }

static int bin_of(u32 size) {
    if (size <= 1024) return size / 16;
    int lg = 31 - __lzcnt(size);  // >= 10
    int b = SMALL_BINS + (lg - 10);
    return b < NUM_BINS ? b : NUM_BINS - 1;
}

static void bin_insert(u32 b) {
    int i = bin_of(bsize(b));
    hdr(b).prev_free = 0;
    hdr(b).next_free = bins[i];
    if (bins[i]) hdr(bins[i]).prev_free = b;
    bins[i] = b;
}

static void bin_remove(u32 b) {
    Hdr& h = hdr(b);
    if (h.prev_free) hdr(h.prev_free).next_free = h.next_free;
    else bins[bin_of(bsize(b))] = h.next_free;
    if (h.next_free) hdr(h.next_free).prev_free = h.prev_free;
}

static void set_size(u32 b, u32 size, bool in_use) {
    hdr(b).size = size | (in_use ? 1 : 0);
    u32 next = b + size;
    if (next < heap_top) hdr(next).prev_size = size;
}

// Split block b (already removed from bins, marked used) down to `need`.
static void split(u32 b, u32 need) {
    u32 total = bsize(b);
    if (total - need < MIN_BLOCK) return;
    set_size(b, need, true);
    u32 rest = b + need;
    hdr(rest).prev_size = need;
    set_size(rest, total - need, false);
    if (last_block == b) last_block = rest;
    bin_insert(rest);
}

u32 alloc(u32 size) {
    if (size > 0x7FFF0000) return 0;
    u32 need = align_up(size, 16) + HDR;
    if (need < MIN_BLOCK) need = MIN_BLOCK;
    std::lock_guard lk(heap_lock);

    for (int i = bin_of(need); i < NUM_BINS; i++) {
        for (u32 b = bins[i]; b; b = hdr(b).next_free) {
            if (bsize(b) < need) continue;
            bin_remove(b);
            hdr(b).size |= 1;
            split(b, need);
            return b + HDR;
        }
    }

    // grow the heap
    if ((u64)heap_top + need > HEAP_END) {
        LOGE("guest heap exhausted (request %u)", size);
        return 0;
    }
    u32 b = heap_top;
    heap_top += need;
    if (heap_top > heap_commit) {
        u32 grow = align_up(heap_top - heap_commit, 16 << 20);
        if ((u64)heap_commit + grow > HEAP_END) grow = HEAP_END - heap_commit;
        commit(heap_commit, grow);
        heap_commit += grow;
        LOGI("guest heap grown to %u MB", (heap_commit - HEAP_BASE) >> 20);
    }
    hdr(b).prev_size = last_block ? bsize(last_block) : 0;
    hdr(b).size = need | 1;
    last_block = b;
    return b + HDR;
}

void free(u32 g) {
    if (!g) return;
    std::lock_guard lk(heap_lock);
    u32 b = g - HDR;
    if (!used(b)) {
        LOGW("double free of %08x", g);
        return;
    }
    u32 size = bsize(b);
    // merge with next
    u32 next = b + size;
    if (next < heap_top && !used(next)) {
        bin_remove(next);
        if (last_block == next) last_block = b;
        size += bsize(next);
    }
    // merge with previous
    if (b != HEAP_BASE) {
        u32 prev = b - hdr(b).prev_size;
        if (!used(prev)) {
            bin_remove(prev);
            if (last_block == b) last_block = prev;
            size += bsize(prev);
            b = prev;
        }
    }
    set_size(b, size, false);
    bin_insert(b);
}

u32 realloc(u32 g, u32 size) {
    if (!g) return alloc(size);
    if (!size) {
        free(g);
        return 0;
    }
    u32 have = bsize(g - HDR) - HDR;
    if (have >= size) return g;
    u32 n = alloc(size);
    if (n) {
        std::memcpy(H(n), H(g), have);
        free(g);
    }
    return n;
}

u32 calloc(u32 n, u32 size) {
    u64 total = (u64)n * size;
    if (total > 0x7FFF0000) return 0;
    u32 g = alloc((u32)total);
    if (g) std::memset(H(g), 0, (size_t)total);
    return g;
}

u32 strdup(const char* s) {
    u32 n = (u32)strlen(s) + 1;
    u32 g = alloc(n);
    std::memcpy(H(g), s, n);
    return g;
}

// ---------------------------------------------------------------------------

static std::mutex map_lock;
static u32 map_top = MAP_BASE;
static u32 data_top = DATA_BASE;

u32 map(u32 size) {
    size = align_up(size, 0x10000);
    std::lock_guard lk(map_lock);
    if ((u64)map_top + size > MAP_END) return 0;
    u32 a = map_top;
    map_top += size;
    commit(a, size);
    std::memset(H(a), 0, size);
    return a;
}

void unmap(u32 addr, u32 size) {
    // Keep it simple: the address range is not reused, but release the RAM.
    VirtualFree(base + addr, align_up(size, 0x1000), MEM_DECOMMIT);
    std::lock_guard lk(commit_lock);
    for (u32 p = addr >> 12; p < (u32)(((u64)addr + size + 0xFFF) >> 12); p++) committed[p] = 0;
}

u32 data_alloc(u32 size, u32 align) {
    std::lock_guard lk(map_lock);
    u32 a = align_up(data_top, align);
    data_top = a + size;
    if (data_top > THUNK_BASE) fatal("HLE data area full");
    commit(a, size);
    std::memset(H(a), 0, size);
    return a;
}

}  // namespace mem
