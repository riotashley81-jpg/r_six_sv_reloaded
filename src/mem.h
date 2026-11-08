// Guest (32-bit ARM) address space. On PC it is one 4 GiB host reservation:
// guest address g lives at host address mem::base + g, so guest pointers can
// be handed to host code after adding the base. On the Vita the game runs
// natively and guest addresses are ordinary pointers.
#pragma once
#include "common.h"

namespace mem {

extern u8* base;

// Fixed layout of the guest address space.
constexpr u32 LIB_BASE   = 0x10000000;  // the game's .so
constexpr u32 THUNK_BASE = 0x0F000000;  // import thunks (SVC stubs)
constexpr u32 DATA_BASE  = 0x0E000000;  // HLE data objects (__sF, ctype tables...)
constexpr u32 HEAP_BASE  = 0x40000000;  // malloc heap, grows up to HEAP_END
constexpr u32 HEAP_END   = 0xB0000000;
constexpr u32 MAP_BASE   = 0xB0000000;  // mmap / thread stacks
constexpr u32 MAP_END    = 0xF0000000;

void init();
void commit(u32 addr, u32 size);
bool valid(u32 addr);

#ifdef __vita__
inline void* H(u32 g) { return (void*)(uintptr_t)g; }
inline u32 G(const void* h) { return (u32)(uintptr_t)h; }
template <class T> inline T& ref(u32 g) { return *(T*)(uintptr_t)g; }
#else
inline void* H(u32 g) { return g ? base + g : nullptr; }
inline u32 G(const void* h) { return h ? (u32)((const u8*)h - base) : 0; }
template <class T> inline T& ref(u32 g) { return *(T*)(base + g); }
#endif
inline u32 rd32(u32 g) { return ref<u32>(g); }
inline void wr32(u32 g, u32 v) { ref<u32>(g) = v; }
inline const char* str(u32 g) { return (const char*)H(g); }

// Guest heap (thread safe).
u32 alloc(u32 size);
void free(u32 g);
u32 realloc(u32 g, u32 size);
u32 calloc(u32 n, u32 size);
u32 strdup(const char* s);

// Page-granular regions (mmap, stacks). Never returned to the heap.
u32 map(u32 size);
void unmap(u32 addr, u32 size);

// Bump allocator for permanent HLE data in DATA_BASE.
u32 data_alloc(u32 size, u32 align = 8);

#ifdef __vita__
// Reserve address space for the game library; commit() then backs parts of
// it with read/write pages, and protect_exec() makes code executable.
u32 reserve_lib(u32 size);
void protect_exec(u32 addr, u32 size);
#endif

}  // namespace mem
