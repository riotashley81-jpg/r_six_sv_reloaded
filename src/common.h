// Shared types and helpers for the Android .so loader.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

void log_line(const char* tag, const char* fmt, ...);
[[noreturn]] void fatal(const char* fmt, ...);

#define LOGI(...) log_line("I", __VA_ARGS__)
#define LOGW(...) log_line("W", __VA_ARGS__)
#define LOGE(...) log_line("E", __VA_ARGS__)

template <class To, class From>
inline To bitcast(const From& f) {
    static_assert(sizeof(To) == sizeof(From));
    To t;
    std::memcpy(&t, &f, sizeof(To));
    return t;
}

inline u32 align_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
