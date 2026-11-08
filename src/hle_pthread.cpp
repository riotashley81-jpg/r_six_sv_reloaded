// pthreads: each guest thread is a host thread (on PC, with its own JIT instance).
#ifdef __vita__
#include <pthread.h>
#endif

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "hle.h"

namespace hle {
namespace {

constexpr u32 THREAD_STACK = 1 << 20;

struct GThread {
#ifdef __vita__
    pthread_t th{};  // std::thread can't set the stack size, and the default is small
    bool started = false;
#else
    std::thread th;
#endif
    u32 start = 0, arg = 0, tid = 0;
    u32 ret = 0;
};

void thread_main(GThread* t) {
    cpu::thread_attach(t->tid, THREAD_STACK);
    u32 r = cpu::call(t->start, {t->arg});
    auto& ts = cpu::thread();
    t->ret = ts.exiting ? ts.exit_value : r;
}
std::mutex threads_lock;
std::unordered_map<u32, GThread*> threads;
std::atomic<u32> next_tid{2};  // 1 = main thread

int h_pthread_create(u32* out, u32 attr, u32 start, u32 arg) {
    u32 tid = next_tid++;
    auto* t = new GThread;
    {
        std::lock_guard lk(threads_lock);
        threads[tid] = t;
    }
    *out = tid;
    t->start = start;
    t->arg = arg;
    t->tid = tid;
    LOGI("pthread_create tid=%u start=%08x arg=%08x", tid, start, arg);
#ifdef __vita__
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, THREAD_STACK);
    t->started = pthread_create(&t->th, &a, [](void* p) -> void* { thread_main((GThread*)p); return nullptr; }, t) == 0;
    pthread_attr_destroy(&a);
    if (!t->started) LOGE("pthread_create failed for tid %u", tid);
#else
    t->th = std::thread(thread_main, t);
#endif
    return 0;
}

int h_pthread_join(u32 tid, u32* ret) {
    GThread* t;
    {
        std::lock_guard lk(threads_lock);
        auto it = threads.find(tid);
        if (it == threads.end()) return 3;  // ESRCH
        t = it->second;
        threads.erase(it);
    }
#ifdef __vita__
    if (t->started) pthread_join(t->th, nullptr);
#else
    if (t->th.joinable()) t->th.join();
#endif
    if (ret) *ret = t->ret;
    delete t;
    return 0;
}

void h_pthread_exit(u32 value) {
    auto& ts = cpu::thread();
    ts.exiting = true;
    ts.exit_value = value;
    cpu::current()->halt();
}

u32 h_pthread_self() { return cpu::thread().tid; }
int h_pthread_kill(u32 tid, int sig) {
    LOGW("pthread_kill(%u, %d) ignored", tid, sig);
    return 0;
}

// --- TLS keys ---------------------------------------------------------------
std::atomic<u32> next_key{1};
thread_local std::unordered_map<u32, u32> tls;
int h_pthread_key_create(u32* key, u32 /*destructor*/) {
    *key = next_key++;
    return 0;
}
u32 h_pthread_getspecific(u32 key) {
    auto it = tls.find(key);
    return it == tls.end() ? 0 : it->second;
}
int h_pthread_setspecific(u32 key, u32 value) {
    tls[key] = value;
    return 0;
}

// --- mutexes ----------------------------------------------------------------
// bionic pthread_mutex_t is a single int that may be statically initialized,
// so host mutexes are created lazily, keyed by guest address. All are
// recursive, which is a superset of what correct code relies on.
std::mutex mutex_map_lock;
std::unordered_map<u32, std::recursive_mutex*> mutexes;

std::recursive_mutex* get_mutex(u32 addr) {
    std::lock_guard lk(mutex_map_lock);
    auto& m = mutexes[addr];
    if (!m) {
        m = new std::recursive_mutex;
        if (mutexes.size() % 500 == 0) LOGI("pthread: %zu mutexes", mutexes.size());
    }
    return m;
}
int h_mutex_init(u32* m, u32) {
    get_mutex(mem::G(m));
    return 0;
}
// Destroying keeps the host mutex: on Android a mutex is just an int, so the
// game may destroy one while another thread (the audio thread, at a
// checkpoint) is still about to lock it, and a deleted host mutex made that
// lock throw and abort(). An address reused for a new mutex gets the same,
// unlocked, host mutex back.
int h_mutex_destroy(u32*) { return 0; }
int h_mutex_lock(u32* m) {
    try {
        get_mutex(mem::G(m))->lock();
    } catch (const std::exception& e) {
        static int warned = 0;
        if (warned++ < 10) LOGW("pthread_mutex_lock(%08x) failed: %s", mem::G(m), e.what());
        return 22;  // EINVAL
    }
    return 0;
}
int h_mutex_trylock(u32* m) { return get_mutex(mem::G(m))->try_lock() ? 0 : 16; }  // EBUSY
int h_mutex_unlock(u32* m) {
    get_mutex(mem::G(m))->unlock();
    return 0;
}

}  // namespace

void init_pthread() {
#define R(name, fn) reg(name, W<fn>)
    R("pthread_create", h_pthread_create); R("pthread_join", h_pthread_join); R("pthread_exit", h_pthread_exit);
    R("pthread_self", h_pthread_self); R("pthread_kill", h_pthread_kill);
    R("pthread_key_create", h_pthread_key_create); R("pthread_getspecific", h_pthread_getspecific);
    R("pthread_setspecific", h_pthread_setspecific);
    R("pthread_mutex_init", h_mutex_init); R("pthread_mutex_destroy", h_mutex_destroy);
    R("pthread_mutex_lock", h_mutex_lock); R("pthread_mutex_trylock", h_mutex_trylock);
    R("pthread_mutex_unlock", h_mutex_unlock);
#undef R
}

}  // namespace hle
