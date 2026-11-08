// bionic libc/libm/libstdc++ subset: strings, stdlib, ctype, math, time,
// ARM EABI helpers, C++ runtime hooks.
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <atomic>
#include <mutex>

#include "hle.h"
#include "os.h"

namespace hle {

std::atomic<u64> virtual_clock_us{0};

void set_errno(int e) {
    u32 a = cpu::thread().errno_addr;
    if (a) mem::wr32(a, (u32)e);
}

namespace {

using wchar = char32_t;  // bionic wchar_t is 32-bit

// --- memory / strings -------------------------------------------------------
void* h_malloc(u32 n) { return mem::H(mem::alloc(n)); }
void h_free(void* p) { mem::free(mem::G(p)); }
void* h_calloc(u32 n, u32 s) { return mem::H(mem::calloc(n, s)); }
void* h_realloc(void* p, u32 n) { return mem::H(mem::realloc(mem::G(p), n)); }

void* h_memcpy(void* d, const void* s, u32 n) { return memcpy(d, s, n); }
void* h_memmove(void* d, const void* s, u32 n) { return memmove(d, s, n); }
void* h_memset(void* d, int c, u32 n) { return memset(d, c, n); }
int h_memcmp(const void* a, const void* b, u32 n) { return memcmp(a, b, n); }
const void* h_memchr(const void* s, int c, u32 n) { return memchr(s, c, n); }
u32 h_strlen(const char* s) { return (u32)strlen(s); }
int h_strcmp(const char* a, const char* b) { return strcmp(a, b); }
int h_strncmp(const char* a, const char* b, u32 n) { return strncmp(a, b, n); }
int h_strcasecmp(const char* a, const char* b) { return os::stricmp(a, b); }
int h_strncasecmp(const char* a, const char* b, u32 n) { return os::strnicmp(a, b, n); }
char* h_strcpy(char* d, const char* s) { return strcpy(d, s); }
char* h_strncpy(char* d, const char* s, u32 n) { return strncpy(d, s, n); }
char* h_strcat(char* d, const char* s) { return strcat(d, s); }
char* h_strncat(char* d, const char* s, u32 n) { return strncat(d, s, n); }
const char* h_strchr(const char* s, int c) { return strchr(s, c); }
const char* h_strrchr(const char* s, int c) { return strrchr(s, c); }
const char* h_strstr(const char* a, const char* b) { return strstr(a, b); }
int h_strcoll(const char* a, const char* b) { return strcmp(a, b); }
u32 h_strlcpy(char* d, const char* s, u32 n) {
    u32 len = (u32)strlen(s);
    if (n) {
        u32 c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

thread_local char* strtok_state;
char* h_strtok(char* s, const char* delim) { return os::strtok_r(s, delim, &strtok_state); }

int h_atoi(const char* s) { return atoi(s); }
s32 h_atol(const char* s) { return atol(s); }
s32 h_strtol(const char* s, u32* end, int base) {
    char* e;
    long v = strtol(s, &e, base);
    if (end) *end = mem::G(e);
    return v;
}
double h_strtod(const char* s, u32* end) {
    char* e;
    double v = strtod(s, &e);
    if (end) *end = mem::G(e);
    return v;
}

int h_toupper(int c) { return toupper(c); }
wchar h_towlower(wchar c) { return c < 0x10000 ? towlower((wint_t)c) : c; }
wchar h_towupper(wchar c) { return c < 0x10000 ? towupper((wint_t)c) : c; }
#define ISW(name) int h_##name(wchar c) { return c < 0x10000 && name((wint_t)c) ? 1 : 0; }
ISW(iswalpha) ISW(iswcntrl) ISW(iswdigit) ISW(iswlower) ISW(iswprint) ISW(iswpunct) ISW(iswspace)
ISW(iswupper) ISW(iswxdigit)
#undef ISW

u32 h_wcslen(const wchar* s) { u32 n = 0; while (s[n]) n++; return n; }
int h_wcscmp(const wchar* a, const wchar* b) {
    while (*a && *a == *b) a++, b++;
    return *a < *b ? -1 : *a > *b;
}
wchar* h_wcscpy(wchar* d, const wchar* s) { wchar* r = d; while ((*d++ = *s++)) {} return r; }
wchar* h_wcsncpy(wchar* d, const wchar* s, u32 n) {
    u32 i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
int h_wmemcmp(const wchar* a, const wchar* b, u32 n) {
    for (u32 i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
wchar* h_wmemcpy(wchar* d, const wchar* s, u32 n) { return (wchar*)memcpy(d, s, n * 4); }
wchar* h_wmemmove(wchar* d, const wchar* s, u32 n) { return (wchar*)memmove(d, s, n * 4); }
wchar* h_wmemset(wchar* d, wchar c, u32 n) { for (u32 i = 0; i < n; i++) d[i] = c; return d; }

// --- stdlib -----------------------------------------------------------------
thread_local u32 qsort_cmp;
int qsort_tramp(const void* a, const void* b) { return (s32)cpu::call(qsort_cmp, {mem::G(a), mem::G(b)}); }
void h_qsort(void* base, u32 n, u32 size, u32 cmp) {
    u32 saved = qsort_cmp;
    qsort_cmp = cmp;
    qsort(base, n, size, qsort_tramp);
    qsort_cmp = saved;
}

// div_t is returned in memory: r0 = result pointer
void h_div(Cpu& c) {
    s32 n = (s32)c.r(1), d = (s32)c.r(2);
    u32 out = c.r(0);
    mem::wr32(out, d ? (u32)(n / d) : 0);
    mem::wr32(out + 4, d ? (u32)(n % d) : 0);
}

u64 rand48_state = 0x1234ABCD330Eull;
void h_srand48(s32 seed) { rand48_state = ((u64)(u32)seed << 16) | 0x330E; }
s32 h_lrand48() {
    rand48_state = (rand48_state * 0x5DEECE66Dull + 0xB) & ((1ull << 48) - 1);
    return (s32)(rand48_state >> 17);
}

u32 h_getenv(const char* name) {
    LOGI("getenv(%s)", name);
    return 0;
}
[[noreturn]] void h_abort() { fatal("guest called abort() lr=%08x", cpu::current()->r(14)); }
[[noreturn]] void h_exit(int code) {
    LOGI("guest called exit(%d)", code);
    os::exit_process(code);
}

u32 basename_buf;
u32 h_basename(const char* path) {
    const char* p = strrchr(path, '/');
    strncpy((char*)mem::H(basename_buf), p ? p + 1 : path, 255);
    return basename_buf;
}
u32 c_locale_str;
u32 h_setlocale(int, const char*) { return c_locale_str; }

int h_chdir(const char* p) { LOGI("chdir(%s)", p); return 0; }
int h_sysconf(int name) {
    switch (name) {
    case 0x27: return 4096;          // _SC_PAGESIZE
    case 0x60: case 0x61: return 2;  // _SC_NPROCESSORS_CONF/ONLN
    case 0x06: return 100;           // _SC_CLK_TCK
    }
    return -1;
}

// --- time -------------------------------------------------------------------
struct GTimeval { s32 sec, usec; };
struct GTimespec { s32 sec, nsec; };
struct GTm { s32 sec, min, hour, mday, mon, year, wday, yday, isdst; s32 gmtoff; u32 zone; };

u64 now_us() {
    if (virtual_clock_us) return virtual_clock_us;
    return os::now_us();
}
// The game's millisecond clock (OS_GetTime, glitch::os::Timer) is
// tv_sec * 1000 + tv_usec / 1000 in 32 bits, which wraps every 49.7 days and
// is negative half the time at today's dates. SoundManagerVox then starts
// with a negative clock, sees every sound's start time as "not playing", and
// restarts looping ambiences every frame, leaking a file and an emitter each
// time. Shift gettimeofday (by under 50 days; time() stays exact) so that
// clock starts at one second, as the game expects.
static u64 gettimeofday_shift_us() {
    static const u64 shift = [] {
        constexpr u64 WRAP_MS = 1ull << 32;
        u64 ms = now_us() / 1000;
        return (ms % WRAP_MS + WRAP_MS - 1000) % WRAP_MS * 1000;
    }();
    return shift;
}
int h_gettimeofday(GTimeval* tv, void*) {
    u64 t = now_us() - gettimeofday_shift_us();
    if (tv) {
        tv->sec = (s32)(t / 1000000);
        tv->usec = (s32)(t % 1000000);
    }
    return 0;
}
s32 h_time(s32* t) {
    s32 v = (s32)(now_us() / 1000000);
    if (t) *t = v;
    return v;
}
void to_gtm(const tm& h, GTm* g) {
    g->sec = h.tm_sec; g->min = h.tm_min; g->hour = h.tm_hour; g->mday = h.tm_mday; g->mon = h.tm_mon;
    g->year = h.tm_year; g->wday = h.tm_wday; g->yday = h.tm_yday; g->isdst = h.tm_isdst;
    g->gmtoff = 0; g->zone = c_locale_str;
}
GTm* h_localtime_r(const s32* t, GTm* out) {
    tm h;
    os::localtime(*t, &h);
    to_gtm(h, out);
    return out;
}
u32 static_tm;
GTm* h_localtime(const s32* t) { return h_localtime_r(t, &mem::ref<GTm>(static_tm)); }
GTm* h_gmtime(const s32* t) {
    tm h;
    os::gmtime(*t, &h);
    to_gtm(h, &mem::ref<GTm>(static_tm));
    return &mem::ref<GTm>(static_tm);
}
s32 h_mktime(GTm* g) {
    tm h{};
    h.tm_sec = g->sec; h.tm_min = g->min; h.tm_hour = g->hour; h.tm_mday = g->mday; h.tm_mon = g->mon;
    h.tm_year = g->year; h.tm_isdst = g->isdst;
    s32 r = (s32)mktime(&h);
    to_gtm(h, g);
    return r;
}
int h_nanosleep(const GTimespec* req, GTimespec* rem) {
    os::sleep_us((u64)req->sec * 1000000 + req->nsec / 1000);
    if (rem) rem->sec = rem->nsec = 0;
    return 0;
}
int h_usleep(u32 us) {
    os::sleep_us(us);
    return 0;
}

// --- math (softfp: floats in core registers) --------------------------------
#define M1F(n) float h_##n(float x) { return ::n(x); }
#define M1D(n) double h_##n(double x) { return ::n(x); }
M1F(acosf) M1F(asinf) M1F(atanf) M1F(ceilf) M1F(cosf) M1F(expf) M1F(floorf) M1F(logf) M1F(sinf) M1F(sqrtf)
M1F(tanf)
M1D(acos) M1D(asin) M1D(atan) M1D(ceil) M1D(cos) M1D(exp) M1D(floor) M1D(log) M1D(log10) M1D(sin)
M1D(sqrt) M1D(tan)
#undef M1F
#undef M1D
float h_atan2f(float y, float x) { return atan2f(y, x); }
float h_fmodf(float a, float b) { return fmodf(a, b); }
float h_powf(float a, float b) { return powf(a, b); }
float h_ldexpf(float a, int e) { return ldexpf(a, e); }
double h_atan2(double y, double x) { return atan2(y, x); }
double h_fmod(double a, double b) { return fmod(a, b); }
double h_pow(double a, double b) { return pow(a, b); }

// --- ARM EABI runtime -------------------------------------------------------
s32 h_idiv(s32 a, s32 b) { return b ? (b == -1 ? (s32)(0u - (u32)a) : a / b) : 0; }
u32 h_uidiv(u32 a, u32 b) { return b ? a / b : 0; }
void h_idivmod(Cpu& c) {
    s32 a = (s32)c.r(0), b = (s32)c.r(1);
    if (!b) { c.r(0) = 0; c.r(1) = (u32)a; return; }
    if (b == -1) { c.r(0) = 0u - (u32)a; c.r(1) = 0; return; }
    c.r(0) = (u32)(a / b);
    c.r(1) = (u32)(a % b);
}
void h_uidivmod(Cpu& c) {
    u32 a = c.r(0), b = c.r(1);
    c.r(0) = b ? a / b : 0;
    c.r(1) = b ? a % b : a;
}
void h_ldivmod(Cpu& c) {
    s64 a = (s64)((u64)c.r(0) | ((u64)c.r(1) << 32)), b = (s64)((u64)c.r(2) | ((u64)c.r(3) << 32));
    s64 q = 0, r = a;
    if (b == -1) q = (s64)(0ull - (u64)a), r = 0;
    else if (b) q = a / b, r = a % b;
    c.ret64((u64)q);
    c.r(2) = (u32)(u64)r;
    c.r(3) = (u32)((u64)r >> 32);
}
void h_uldivmod(Cpu& c) {
    u64 a = (u64)c.r(0) | ((u64)c.r(1) << 32), b = (u64)c.r(2) | ((u64)c.r(3) << 32);
    u64 q = b ? a / b : 0, r = b ? a % b : a;
    c.ret64(q);
    c.r(2) = (u32)r;
    c.r(3) = (u32)(r >> 32);
}
s32 h_d2iz(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (s32)d;
}
s64 h_d2lz(double d) {
    if (d != d) return 0;
    if (d >= 9.2233720368547758e18) return INT64_MAX;
    if (d <= -9.2233720368547758e18) return INT64_MIN;
    return (s64)d;
}
double h_dadd(double a, double b) { return a + b; }
double h_ddiv(double a, double b) { return a / b; }
double h_dmul(double a, double b) { return a * b; }
double h_i2d(s32 a) { return a; }
double h_ul2d(u64 a) { return (double)a; }

// --- C++ runtime ------------------------------------------------------------
std::recursive_mutex guard_lock;
int h_cxa_guard_acquire(u32* g) {
    if (*g & 1) return 0;
    guard_lock.lock();
    if (*g & 1) {
        guard_lock.unlock();
        return 0;
    }
    return 1;
}
void h_cxa_guard_release(u32* g) {
    *g = 1;
    guard_lock.unlock();
}
int h_aeabi_atexit(u32, u32, u32) { return 0; }  // destructors never run; the process just exits
[[noreturn]] void h_pure_virtual() { fatal("pure virtual call lr=%08x", cpu::current()->r(14)); }
[[noreturn]] void h_stack_chk_fail() { fatal("stack smashing detected lr=%08x", cpu::current()->r(14)); }
[[noreturn]] void h_unwind() { fatal("C++ exception unwinding is not supported (lr=%08x)", cpu::current()->r(14)); }
u32 h_errno_loc() { return cpu::thread().errno_addr; }

// --- setjmp/longjmp -----------------------------------------------------------
// jmp_buf layout (ours): r4-r11, sp, lr, d8-d15, fpscr
void h_setjmp(Cpu& c) {
    u32 b = c.r(0);
    for (int i = 0; i < 8; i++) mem::wr32(b + i * 4, c.r(4 + i));
    mem::wr32(b + 32, c.r(13));
    mem::wr32(b + 36, c.r(14));
    for (int i = 0; i < 16; i++) mem::wr32(b + 40 + i * 4, c.ext(16 + i));
    mem::wr32(b + 104, c.fpscr());
    c.r(0) = 0;
}
void h_longjmp(Cpu& c) {
    u32 b = c.r(0), v = c.r(1);
    for (int i = 0; i < 8; i++) c.r(4 + i) = mem::rd32(b + i * 4);
    c.r(13) = mem::rd32(b + 32);
    c.r(14) = mem::rd32(b + 36);  // the thunk's BX LR returns to setjmp's caller
    for (int i = 0; i < 16; i++) c.ext(16 + i) = mem::rd32(b + 40 + i * 4);
    c.set_fpscr(mem::rd32(b + 104));
    c.r(0) = v ? v : 1;
}

int h_sigaction(int, u32, u32) { return 0; }
int h_android_log_write(int prio, const char* tag, const char* msg) {
    log_line(prio >= 6 ? "E" : prio == 5 ? "W" : "I", "[%s] %s", tag, msg);
    return 1;
}

// bionic ctype flags
enum { CU = 0x01, CL = 0x02, CN = 0x04, CS = 0x08, CP = 0x10, CC = 0x20, CX = 0x40, CB = 0x80 };

// bionic exports these as pointers to the tables (const char* _ctype_)
void reg_table_ptr(const char* name, u32 table) {
    u32 cell = mem::data_alloc(4);
    mem::wr32(cell, table);
    reg_data(name, cell);
}

void make_ctype_tables() {
    u32 ct = mem::data_alloc(257, 4);
    u8* t = (u8*)mem::H(ct) + 1;
    for (int c = 0; c < 128; c++) {
        u8 f = 0;
        if (isupper(c)) f |= CU;
        if (islower(c)) f |= CL;
        if (isdigit(c)) f |= CN;
        if (isspace(c)) f |= CS;
        if (ispunct(c)) f |= CP;
        if (iscntrl(c)) f |= CC;
        if (isxdigit(c)) f |= CX;
        if (c == ' ') f |= CB;
        t[c] = f;
    }
    reg_table_ptr("_ctype_", ct);

    u32 lo = mem::data_alloc(257 * 2, 4), up = mem::data_alloc(257 * 2, 4);
    s16* l = (s16*)mem::H(lo);
    s16* u = (s16*)mem::H(up);
    l[0] = u[0] = -1;
    for (int c = 0; c < 256; c++) {
        l[c + 1] = (s16)(c < 128 ? tolower(c) : c);
        u[c + 1] = (s16)(c < 128 ? toupper(c) : c);
    }
    reg_table_ptr("_tolower_tab_", lo);
    reg_table_ptr("_toupper_tab_", up);
}

}  // namespace

void init_libc() {
    make_ctype_tables();
    basename_buf = mem::data_alloc(256);
    c_locale_str = mem::data_alloc(4);
    strcpy((char*)mem::H(c_locale_str), "C");
    static_tm = mem::data_alloc(sizeof(GTm));

    u32 guard = mem::data_alloc(4);
    mem::wr32(guard, 0x5EC0DE42);
    reg_data("__stack_chk_guard", guard);
    reg_data("__dso_handle", mem::data_alloc(4));
    reg_data("_ZSt7nothrow", mem::data_alloc(4));

#define R(name, fn) reg(name, W<fn>)
    R("malloc", h_malloc); R("free", h_free); R("calloc", h_calloc); R("realloc", h_realloc);
    R("_Znwj", h_malloc); R("_Znaj", h_malloc); R("_ZnajRKSt9nothrow_t", h_malloc);
    R("_ZdlPv", h_free); R("_ZdaPv", h_free);
    R("memcpy", h_memcpy); R("memmove", h_memmove); R("memset", h_memset); R("memcmp", h_memcmp);
    R("memchr", h_memchr); R("strlen", h_strlen); R("strcmp", h_strcmp); R("strncmp", h_strncmp);
    R("strcasecmp", h_strcasecmp); R("strncasecmp", h_strncasecmp); R("strcpy", h_strcpy);
    R("strncpy", h_strncpy); R("strcat", h_strcat); R("strncat", h_strncat); R("strchr", h_strchr);
    R("strrchr", h_strrchr); R("strstr", h_strstr); R("strcoll", h_strcoll); R("strlcpy", h_strlcpy);
    R("strtok", h_strtok); R("atoi", h_atoi); R("atol", h_atol); R("strtol", h_strtol); R("strtod", h_strtod);
    R("toupper", h_toupper); R("towlower", h_towlower); R("towupper", h_towupper);
    R("iswalpha", h_iswalpha); R("iswcntrl", h_iswcntrl); R("iswdigit", h_iswdigit); R("iswlower", h_iswlower);
    R("iswprint", h_iswprint); R("iswpunct", h_iswpunct); R("iswspace", h_iswspace); R("iswupper", h_iswupper);
    R("iswxdigit", h_iswxdigit);
    R("wcslen", h_wcslen); R("wcscmp", h_wcscmp); R("wcscpy", h_wcscpy); R("wcsncpy", h_wcsncpy);
    R("wmemcmp", h_wmemcmp); R("wmemcpy", h_wmemcpy); R("wmemmove", h_wmemmove); R("wmemset", h_wmemset);
    R("qsort", h_qsort); reg("div", h_div); R("srand48", h_srand48); R("lrand48", h_lrand48);
    R("getenv", h_getenv); R("abort", h_abort); R("exit", h_exit); R("basename", h_basename);
    R("setlocale", h_setlocale); R("chdir", h_chdir); R("sysconf", h_sysconf);
    R("gettimeofday", h_gettimeofday); R("time", h_time); R("localtime", h_localtime);
    R("localtime_r", h_localtime_r); R("gmtime", h_gmtime); R("mktime", h_mktime);
    R("nanosleep", h_nanosleep); R("usleep", h_usleep);
    R("acosf", h_acosf); R("asinf", h_asinf); R("atanf", h_atanf); R("ceilf", h_ceilf); R("cosf", h_cosf);
    R("expf", h_expf); R("floorf", h_floorf); R("logf", h_logf); R("sinf", h_sinf); R("sqrtf", h_sqrtf);
    R("tanf", h_tanf); R("acos", h_acos); R("asin", h_asin); R("atan", h_atan); R("ceil", h_ceil);
    R("cos", h_cos); R("exp", h_exp); R("floor", h_floor); R("log", h_log); R("log10", h_log10);
    R("sin", h_sin); R("sqrt", h_sqrt); R("tan", h_tan); R("atan2f", h_atan2f); R("fmodf", h_fmodf);
    R("powf", h_powf); R("ldexpf", h_ldexpf); R("atan2", h_atan2); R("fmod", h_fmod); R("pow", h_pow);
    R("__aeabi_idiv", h_idiv); R("__aeabi_uidiv", h_uidiv); reg("__aeabi_idivmod", h_idivmod);
    reg("__aeabi_uidivmod", h_uidivmod); reg("__aeabi_ldivmod", h_ldivmod); reg("__aeabi_uldivmod", h_uldivmod);
    R("__aeabi_d2iz", h_d2iz); R("__aeabi_d2lz", h_d2lz); R("__aeabi_dadd", h_dadd); R("__aeabi_ddiv", h_ddiv);
    R("__aeabi_dmul", h_dmul); R("__aeabi_i2d", h_i2d); R("__aeabi_ul2d", h_ul2d);
    R("__cxa_guard_acquire", h_cxa_guard_acquire); R("__cxa_guard_release", h_cxa_guard_release);
    R("__aeabi_atexit", h_aeabi_atexit); R("__cxa_pure_virtual", h_pure_virtual);
    R("__stack_chk_fail", h_stack_chk_fail); R("__aeabi_unwind_cpp_pr0", h_unwind);
    R("__aeabi_unwind_cpp_pr1", h_unwind); R("__errno", h_errno_loc);
    reg("setjmp", h_setjmp); reg("longjmp", h_longjmp);
    R("sigaction", h_sigaction); R("__android_log_write", h_android_log_write);
#undef R
}

}  // namespace hle
