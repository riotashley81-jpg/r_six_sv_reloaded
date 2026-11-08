// Registry of host implementations for the .so's imports.
#pragma once
#include <atomic>

#include "abi.h"

namespace hle {

void reg(const char* name, HleFn fn);
void reg_data(const char* name, u32 guest_addr);

// Guest address for an imported symbol: a thunk, a data object, or a
// logging stub for anything not implemented.
u32 resolve(const char* name);

void init();  // registers every module below

void init_libc();
void init_stdio();
void init_pthread();
void init_net();
void init_gl_null();     // headless: fake GL state, draws nothing
void init_gl_desktop();  // needs a current desktop GL context
void init_gl_vita();     // needs vitaGL initialized
// Vita: bind ABI-compatible imports (memcpy, malloc, __aeabi_*...) straight
// to the native functions instead of going through a thunk.
void init_native_direct();
// The last file operations (for crash reports).
void dump_file_ops(void (*out)(const char*));
extern bool headless;

// When non-zero, the guest's clock (gettimeofday/time) reads this instead of
// the real time; scripted runs advance it per frame for deterministic replay.
extern std::atomic<u64> virtual_clock_us;

// guest errno for the current thread
void set_errno(int e);

// Map an Android path to a host path (game data lives under the data root).
std::string host_path(const char* guest_path);
void set_data_root(const std::string& root);

}  // namespace hle
