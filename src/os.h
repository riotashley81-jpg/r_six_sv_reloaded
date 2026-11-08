// The few OS services the shared HLE code needs, for Windows and the Vita.
#pragma once
#include "common.h"

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#else
#include <windows.h>
#endif

namespace os {

[[noreturn]] inline void exit_process(int code) {
    fflush(stderr);
#ifdef __vita__
    sceKernelExitProcess(code);
    for (;;) {}
#else
    ExitProcess((UINT)code);
#endif
}

inline void sleep_us(u64 us) {
#ifdef __vita__
    sceKernelDelayThread((SceUInt)us);
#else
    Sleep((DWORD)(us / 1000));
#endif
}

// wall clock, microseconds since 1970
inline u64 now_us() {
#ifdef __vita__
    timeval tv;
    gettimeofday(&tv, nullptr);
    return (u64)tv.tv_sec * 1000000 + (u64)tv.tv_usec;
#else
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10 - 11644473600000000ull;
#endif
}

inline int stricmp(const char* a, const char* b) {
#ifdef __vita__
    return strcasecmp(a, b);
#else
    return _stricmp(a, b);
#endif
}
inline int strnicmp(const char* a, const char* b, size_t n) {
#ifdef __vita__
    return strncasecmp(a, b, n);
#else
    return _strnicmp(a, b, n);
#endif
}
inline char* strtok_r(char* s, const char* delim, char** state) {
#ifdef __vita__
    return ::strtok_r(s, delim, state);
#else
    return strtok_s(s, delim, state);
#endif
}
inline void localtime(time_t t, tm* out) {
#ifdef __vita__
    localtime_r(&t, out);
#else
    localtime_s(out, &t);
#endif
}
inline void gmtime(time_t t, tm* out) {
#ifdef __vita__
    gmtime_r(&t, out);
#else
    gmtime_s(out, &t);
#endif
}

}  // namespace os
