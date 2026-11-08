// ARM CPU execution via dynarmic (PC) or natively (Vita), plus the
// host<->guest calling convention.
#pragma once
#include <initializer_list>
#include <memory>

#include "common.h"
#include "mem.h"

struct Cpu;
using HleFn = void (*)(Cpu&);

#ifdef __vita__
// Registers the thunk trampoline (thunks.S) saved on entry. Handlers read and
// write them; the trampoline restores them on return, so longjmp-style
// changes to r4-r11, sp, lr and d8-d15 take effect.
struct ThunkFrame {
    u32 r[16];   // r0-r12 as passed, sp at the call, lr, (unused)
    u32 s[16];   // s16-s31 (d8-d15)
    u32 fpscr, pad;
};

struct Cpu {
    ThunkFrame* f = nullptr;
    u32 dummy = 0;

    u32& r(int i) { return f->r[i]; }
    u32 sp() { return f->r[13]; }
    u32 arg(int i) { return i < 4 ? f->r[i] : *(u32*)(uintptr_t)(f->r[13] + 4 * (i - 4)); }
    u32& ext(int i) { return i >= 16 && i < 32 ? f->s[i - 16] : dummy; }
    u32 fpscr() { return f->fpscr; }
    void set_fpscr(u32 v) { f->fpscr = v; }
    [[noreturn]] void halt();
    void ret(u32 v) { r(0) = v; }
    void ret64(u64 v) { r(0) = (u32)v; r(1) = (u32)(v >> 32); }
};
#else
namespace Dynarmic::A32 { class Jit; }

struct Cpu {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::unique_ptr<struct CpuCallbacks> cb;
    u32 id;
    bool returned = false;  // set by the return thunk

    Cpu();
    ~Cpu();

    u32& r(int i);
    u32 sp();
    u32 arg(int i);  // i-th 32-bit argument (r0-r3, then stack)
    u32& ext(int i);  // VFP registers as 32-bit words (s0-s63)
    u32 fpscr();
    void set_fpscr(u32 v);
    void halt();
    void ret(u32 v) { r(0) = v; }
    void ret64(u64 v) { r(0) = (u32)v; r(1) = (u32)(v >> 32); }
};
#endif

namespace cpu {

void init();

// Register a host handler; returns the guest address of an ARM thunk that
// calls it (SVC #n; BX LR).
u32 make_thunk(HleFn fn, const char* name);
const char* thunk_name(u32 svc);
u32 current_svc();  // SVC number of the handler currently running
#ifndef __vita__
extern bool trace_gl_calls;  // log each gl* import call (PC, R6SV_TRACEFRAME)
#endif
#ifdef __vita__
u32 last_svc();     // last import this thread called (for crash reports)
#endif

// Call a guest function from host code on the current thread. Re-entrant:
// calls made from inside a handler use a fresh JIT instance.
u32 call(u32 fn, std::initializer_list<u32> args = {});
u64 call64(u32 fn, std::initializer_list<u32> args = {});

// Per-thread guest state.
struct ThreadState {
    u32 stack_top = 0;
    u32 errno_addr = 0;
    u32 tid = 0;
    bool exiting = false;
    u32 exit_value = 0;
};
ThreadState& thread();
void thread_attach(u32 tid, u32 stack_size);  // must run on the new host thread
Cpu* current();

}  // namespace cpu
