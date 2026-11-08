#include "cpu.h"

#include <atomic>
#include <mutex>
#include <vector>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

using namespace Dynarmic;

static constexpr u32 MAX_CPUS = 256;
static constexpr u32 RETURN_SVC = 0;

static std::unique_ptr<ExclusiveMonitor> monitor;
static std::vector<HleFn> handlers;
static std::vector<std::string> names;
static std::mutex thunk_lock;
static std::mutex id_lock;
static std::vector<u32> free_ids;

static thread_local cpu::ThreadState tstate;
static thread_local std::vector<std::unique_ptr<Cpu>> pool;
static thread_local int depth = 0;
static thread_local u32 last_svc = 0;

bool cpu::trace_gl_calls = false;
static const bool trace_shader_calls = getenv("R6SV_TRACESHADERS") != nullptr;

// One line per gl* call: raw argument registers, plus the floats behind the
// pointer argument of the *fv state calls.
static void trace_call(u32 swi) {
    const std::string& n = names[swi];
    if (n.compare(0, 2, "gl") != 0) return;
    Cpu* c = cpu::current();
    char extra[96] = "";
    int ptr_arg = n == "glLightModelfv" ? 1
                  : (n == "glTexEnvfv" || n == "glLightfv" || n == "glMaterialfv" || n == "glFogfv") ? 2 : -1;
    if (n == "glFogfv") ptr_arg = 1;
    if ((n == "glLoadMatrixf" || n == "glMultMatrixf") && mem::valid(c->r(0))) {
        const float* m = (const float*)mem::H(c->r(0));
        LOGI("GL %s {%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g}", n.c_str(), m[0],
             m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        return;
    }
    if (n == "glTexCoordPointer" && c->r(1) == 0x1402 && mem::valid(c->r(3))) {
        // first three vertices of a GL_SHORT stream, as shorts from the pointer and from the vertex start
        const s16* p = (const s16*)mem::H(c->r(3));
        u32 stride = c->r(2);
        for (int v = 0; v < 3; v++) {
            const s16* q = (const s16*)((const u8*)p + v * stride);
            LOGI("GL   texcoord v%d: %d %d | next: %d %d %d %d", v, q[0], q[1], q[2], q[3], q[4], q[5]);
        }
    }
    if (ptr_arg >= 0 && mem::valid(c->r(ptr_arg))) {
        const float* v = (const float*)mem::H(c->r(ptr_arg));
        snprintf(extra, sizeof extra, " {%.3f %.3f %.3f %.3f}", v[0], v[1], v[2], v[3]);
    }
    auto f = [&](int i) { float x; u32 b = c->r(i); std::memcpy(&x, &b, 4); return x; };
    LOGI("GL %s(%x, %x, %x, %x) [as float: %g %g %g %g]%s", n.c_str(), c->r(0), c->r(1), c->r(2), c->r(3), f(0), f(1),
         f(2), f(3), extra);
}

static void bad_access(const char* kind, u32 addr) {
    Cpu* c = cpu::current();
    LOGE("bad %s at %08x (pc=%08x lr=%08x)", kind, addr, c ? c->r(15) : 0, c ? c->r(14) : 0);
}

struct CpuCallbacks final : A32::UserCallbacks {
    Cpu* owner;
    explicit CpuCallbacks(Cpu* c) : owner(c) {}

    template <class T> T read(u32 a) {
        if (!mem::valid(a) || !mem::valid(a + sizeof(T) - 1)) {
            bad_access("read", a);
            return 0;
        }
        T v;
        std::memcpy(&v, mem::base + a, sizeof(T));
        return v;
    }
    template <class T> void write(u32 a, T v) {
        if (!mem::valid(a) || !mem::valid(a + sizeof(T) - 1)) {
            bad_access("write", a);
            return;
        }
        std::memcpy(mem::base + a, &v, sizeof(T));
    }
    template <class T> bool write_excl(u32 a, T v, T expected) {
        if (!mem::valid(a)) {
            bad_access("exclusive write", a);
            return false;
        }
        return std::atomic_ref<T>(mem::ref<T>(a)).compare_exchange_strong(expected, v);
    }

    u8 MemoryRead8(u32 a) override { return read<u8>(a); }
    u16 MemoryRead16(u32 a) override { return read<u16>(a); }
    u32 MemoryRead32(u32 a) override { return read<u32>(a); }
    u64 MemoryRead64(u32 a) override { return read<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { write(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { write(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { write(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { write(a, v); }
    bool MemoryWriteExclusive8(u32 a, u8 v, u8 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive16(u32 a, u16 v, u16 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive32(u32 a, u32 v, u32 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive64(u32 a, u64 v, u64 e) override { return write_excl(a, v, e); }

    void InterpreterFallback(u32 pc, size_t n) override { fatal("interpreter fallback at %08x (%zu)", pc, n); }

    void CallSVC(u32 swi) override {
        if (swi >= handlers.size()) fatal("unknown SVC %u at %08x", swi, owner->r(15));
        u32 saved = last_svc;
        last_svc = swi;
        if (cpu::trace_gl_calls) trace_call(swi);
        else if (trace_shader_calls) {
            // R6SV_TRACESHADERS: log shader and program calls at any time
            const std::string& n = names[swi];
            if (n.find("Shader") != std::string::npos || n.find("Program") != std::string::npos ||
                n.find("GetActive") != std::string::npos || n.find("Location") != std::string::npos)
                trace_call(swi);
        }
        handlers[swi](*owner);
        last_svc = saved;
    }

    void ExceptionRaised(u32 pc, A32::Exception e) override {
        using E = A32::Exception;
        switch (e) {
        case E::Yield: case E::SendEvent: case E::SendEventLocal: case E::WaitForEvent:
        case E::WaitForInterrupt: case E::PreloadData: case E::PreloadDataWithIntentToWrite:
        case E::PreloadInstruction:
            return;
        default:
            fatal("CPU exception %d at pc=%08x lr=%08x", (int)e, pc, owner->r(14));
        }
    }

    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return ~0ull >> 2; }
};

Cpu::Cpu() = default;
Cpu::~Cpu() {
    std::lock_guard lk(id_lock);
    free_ids.push_back(id);
}

u32& Cpu::r(int i) { return jit->Regs()[i]; }
u32 Cpu::sp() { return r(13); }
u32& Cpu::ext(int i) { return jit->ExtRegs()[i]; }
u32 Cpu::fpscr() { return jit->Fpscr(); }
void Cpu::set_fpscr(u32 v) { jit->SetFpscr(v); }
void Cpu::halt() { jit->HaltExecution(); }
u32 Cpu::arg(int i) { return i < 4 ? r(i) : mem::rd32(sp() + 4 * (i - 4)); }

static std::unique_ptr<Cpu> make_cpu(size_t code_cache) {
    auto c = std::make_unique<Cpu>();
    {
        std::lock_guard lk(id_lock);
        if (free_ids.empty()) fatal("too many guest CPUs");
        c->id = free_ids.back();
        free_ids.pop_back();
    }
    c->cb = std::make_unique<CpuCallbacks>(c.get());
    A32::UserConfig cfg;
    cfg.callbacks = c->cb.get();
    cfg.processor_id = c->id;
    cfg.global_monitor = monitor.get();
    cfg.arch_version = A32::ArchVersion::v7;
    cfg.fastmem_pointer = (uintptr_t)mem::base;
    cfg.enable_cycle_counting = false;
    cfg.code_cache_size = code_cache;
    c->jit = std::make_unique<A32::Jit>(cfg);
    return c;
}

namespace cpu {

void init() {
    monitor = std::make_unique<ExclusiveMonitor>(MAX_CPUS);
    for (u32 i = MAX_CPUS; i-- > 0;) free_ids.push_back(i);
    mem::commit(mem::THUNK_BASE, 0x100000);
    make_thunk([](Cpu& c) {
        c.returned = true;
        c.jit->HaltExecution();
    }, "<return>");
}

u32 make_thunk(HleFn fn, const char* name) {
    std::lock_guard lk(thunk_lock);
    u32 n = (u32)handlers.size();
    if (n * 8 >= 0x100000) fatal("thunk area full");
    handlers.push_back(fn);
    names.emplace_back(name);
    u32 a = mem::THUNK_BASE + n * 8;
    mem::wr32(a, 0xEF000000 | n);  // SVC #n
    mem::wr32(a + 4, 0xE12FFF1E);  // BX LR
    return a;
}

const char* thunk_name(u32 svc) { return svc < names.size() ? names[svc].c_str() : "?"; }
u32 current_svc() { return last_svc; }

u64 call64(u32 fn, std::initializer_list<u32> args) {
    if (depth == (int)pool.size()) pool.push_back(make_cpu(depth == 0 ? (64 << 20) : (16 << 20)));
    Cpu& c = *pool[depth];
    auto& R = c.jit->Regs();

    u32 sp = depth == 0 ? tstate.stack_top : ((pool[depth - 1]->sp() - 256) & ~7u);
    u32 nstack = args.size() > 4 ? (u32)args.size() - 4 : 0;
    sp = (sp - nstack * 4) & ~7u;
    int i = 0;
    for (u32 a : args) {
        if (i < 4) R[i] = a;
        else mem::wr32(sp + 4 * (i - 4), a);
        i++;
    }
    R[13] = sp;
    R[14] = mem::THUNK_BASE + RETURN_SVC * 8;
    R[15] = fn & ~1u;
    c.jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));
    c.jit->SetFpscr(0);
    c.returned = false;

    depth++;
    while (!c.returned && !tstate.exiting) c.jit->Run();
    c.jit->ClearHalt();
    depth--;
    return (u64)R[0] | ((u64)R[1] << 32);
}

u32 call(u32 fn, std::initializer_list<u32> args) { return (u32)call64(fn, args); }

ThreadState& thread() { return tstate; }

void thread_attach(u32 tid, u32 stack_size) {
    u32 stack = mem::map(stack_size);
    if (!stack) fatal("cannot allocate guest stack");
    tstate.stack_top = stack + stack_size - 16;
    tstate.errno_addr = mem::alloc(4);
    tstate.tid = tid;
}

Cpu* current() { return depth > 0 ? pool[depth - 1].get() : nullptr; }

}  // namespace cpu
