// Vita memory: guest addresses are native pointers. The game library lives in
// kubridge memory blocks (so its code can be made executable); everything
// else comes from the newlib heap.
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <kubridge.h>
#include <psp2/kernel/sysmem.h>

#include "mem.h"

// main memory, user read+execute (MAIN_R with the execute bit); the SDK has no name for it
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050

namespace mem {

u8* base = nullptr;

void init() {}

// The library is placed the way Vita so-loader ports place theirs (plain
// memory blocks at a fixed address, allocated through kubridge). The library
// is first loaded and relocated in one read/write block; protect_exec() then
// splits it into a read/execute block for the code and a read/write block for
// the rest. (kubridge's reserve/commit/protect calls did the same with less
// copying, but the console froze whenever the process ended.)
static const u32 LIB_ADDR = 0x98000000;
static u32 lib_end = 0;
static SceUID lib_block = -1;

static SceUID alloc_at(const char* name, SceKernelMemBlockType type, u32 addr, u32 size) {
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.size = sizeof opt;
    opt.attr = 0x1;        // fixed address
    opt.field_C = addr;
    SceUID id = kuKernelAllocMemBlock(name, type, size, &opt);
    if (id < 0) fatal("cannot allocate %s at %08x (%u KiB): 0x%08x. Is kubridge.skprx installed?", name, addr, size / 1024, (u32)id);
    return id;
}

u32 reserve_lib(u32) { return LIB_ADDR; }

void commit(u32 addr, u32 size) {
    if (addr != LIB_ADDR || lib_block >= 0) fatal("unexpected commit(%08x, %u)", addr, size);
    lib_end = align_up(addr + size, 0x1000);
    lib_block = alloc_at("r6sv_lib", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, addr, lib_end - addr);
    LOGI("game library at %08x (%u KiB)", addr, (lib_end - addr) / 1024);
}

void protect_exec(u32 addr, u32 size) {
    u32 text_end = align_up(addr + size, 0x1000);
    if (addr != LIB_ADDR || lib_block < 0) fatal("unexpected protect_exec(%08x, %u)", addr, size);
    std::vector<u8> image((u8*)(uintptr_t)addr, (u8*)(uintptr_t)lib_end);
    sceKernelFreeMemBlock(lib_block);
    lib_block = -1;
    alloc_at("r6sv_text", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, addr, text_end - addr);
    int r = kuKernelCpuUnrestrictedMemcpy((void*)(uintptr_t)addr, image.data(), text_end - addr);
    if (r < 0) fatal("cannot write the game code: 0x%08x", (u32)r);
    kuKernelFlushCaches((void*)(uintptr_t)addr, text_end - addr);
    if (text_end < lib_end) {
        alloc_at("r6sv_data", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, text_end, lib_end - text_end);
        memcpy((void*)(uintptr_t)text_end, image.data() + (text_end - addr), lib_end - text_end);
    }
}

bool valid(u32 addr) { return addr != 0; }

u32 alloc(u32 size) { return G(::malloc(size ? size : 1)); }
void free(u32 g) { ::free(H(g)); }
u32 realloc(u32 g, u32 size) { return G(::realloc(H(g), size)); }
u32 calloc(u32 n, u32 size) { return G(::calloc(n, size)); }
u32 strdup(const char* s) { return G(::strdup(s)); }

u32 map(u32 size) {
    void* p = memalign(0x1000, align_up(size, 0x1000));
    if (p) memset(p, 0, size);  // mmap'd memory starts zeroed
    return G(p);
}
void unmap(u32 addr, u32) { ::free(H(addr)); }

u32 data_alloc(u32 size, u32 align) {
    void* p = memalign(align < 4 ? 4 : align, size ? size : 4);
    if (!p) fatal("out of memory (data_alloc %u)", size);
    memset(p, 0, size);
    return G(p);
}

}  // namespace mem
