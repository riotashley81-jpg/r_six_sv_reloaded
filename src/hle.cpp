#include "hle.h"

#include <mutex>
#include <unordered_map>

namespace hle {

struct Entry {
    HleFn fn = nullptr;
    u32 data = 0;
    u32 thunk = 0;
};
static std::unordered_map<std::string, Entry>& table() {
    static std::unordered_map<std::string, Entry> t;
    return t;
}

void reg(const char* name, HleFn fn) { table()[name].fn = fn; }
void reg_data(const char* name, u32 addr) { table()[name].data = addr; }

static void missing(Cpu& c) {
    static std::mutex lk;
    static std::unordered_map<u32, int> seen;
    u32 svc = cpu::current_svc();
    std::lock_guard g(lk);
    if (seen[svc]++ < 3)
        LOGW("unimplemented import %s(%08x, %08x, %08x, %08x) lr=%08x", cpu::thunk_name(svc), c.r(0), c.r(1),
             c.r(2), c.r(3), c.r(14));
    c.r(0) = 0;
}

u32 resolve(const char* name) {
    Entry& e = table()[name];
    if (e.data) return e.data;
    if (!e.thunk) e.thunk = cpu::make_thunk(e.fn ? e.fn : missing, name);
    if (!e.fn) LOGW("import %s is not implemented", name);
    return e.thunk;
}

bool headless = false;

void init() {
    init_libc();
    init_stdio();
    init_pthread();
    init_net();
#ifdef __vita__
    init_gl_vita();
#else
    if (headless) init_gl_null();
    else init_gl_desktop();
#endif
}

}  // namespace hle
