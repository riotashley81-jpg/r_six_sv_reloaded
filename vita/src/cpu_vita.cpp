// Native execution on the Vita: the game's ARM code runs directly; imports
// reach host handlers through the trampolines in thunks.S.
#include <csetjmp>
#include <mutex>

#include "cpu.h"

extern "C" char r6_thunk_table[];

namespace {

constexpr u32 MAX_THUNKS = 1024;  // entries in thunks.S
constexpr u32 THUNK_BYTES = 8;    // movw + b

struct Thunk {
    HleFn fn;
    const char* name;
};
Thunk thunks[MAX_THUNKS];
u32 nthunks = 0;
std::mutex thunk_lock;

thread_local Cpu* t_current = nullptr;
thread_local u32 t_svc = 0;
thread_local u32 t_last = ~0u;
thread_local cpu::ThreadState t_state;
thread_local int t_depth = 0;
thread_local jmp_buf t_exit;  // outermost cpu::call on this thread, for pthread_exit

}  // namespace

extern "C" void r6_thunk_dispatch(ThunkFrame* f, u32 index) {
    Cpu c;
    c.f = f;
    Cpu* prev = t_current;
    u32 prev_svc = t_svc;
    t_current = &c;
    t_svc = index;
    t_last = index;
    thunks[index].fn(c);
    t_current = prev;
    t_svc = prev_svc;
}

void Cpu::halt() { longjmp(t_exit, 1); }

namespace cpu {

void init() {}

u32 make_thunk(HleFn fn, const char* name) {
    std::lock_guard lk(thunk_lock);
    if (nthunks == MAX_THUNKS) fatal("out of import thunks (%u)", MAX_THUNKS);
    thunks[nthunks] = {fn, name};
    return (u32)(uintptr_t)(r6_thunk_table + THUNK_BYTES * nthunks++);
}

const char* thunk_name(u32 svc) { return svc < nthunks ? thunks[svc].name : "?"; }
u32 current_svc() { return t_svc; }
u32 last_svc() { return t_last; }

// Integer arguments and a 64-bit result use the same registers in the game's
// softfp ABI and the Vita's hard-float one, so a plain call works. Unused
// argument slots are harmless extra registers/stack words.
using GuestFn = u64 (*)(u32, u32, u32, u32, u32, u32, u32, u32);

u64 call64(u32 fn, std::initializer_list<u32> args) {
    u32 a[8] = {};
    int n = 0;
    for (u32 v : args) {
        if (n == 8) fatal("cpu::call: too many arguments");
        a[n++] = v;
    }
    if (t_depth == 0 && setjmp(t_exit)) {  // pthread_exit from inside the call
        t_depth = 0;
        t_current = nullptr;
        t_svc = 0;
        return 0;
    }
    t_depth++;
    u64 r = ((GuestFn)(uintptr_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    t_depth--;
    return r;
}

u32 call(u32 fn, std::initializer_list<u32> args) { return (u32)call64(fn, args); }

ThreadState& thread() { return t_state; }

void thread_attach(u32 tid, u32 /*stack_size: native threads own their stacks*/) {
    t_state.tid = tid;
    t_state.errno_addr = mem::data_alloc(4);
}

Cpu* current() { return t_current; }

}  // namespace cpu
