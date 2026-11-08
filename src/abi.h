// AAPCS (softfp) argument marshalling between guest registers/stack and host
// C++ functions. Pointer parameters are translated guest->host automatically;
// pointer results must point into guest memory.
//
// Note: never use size_t/long long for 32-bit guest values in HLE signatures;
// any 8-byte integer is read as a 64-bit (register-pair) argument.
#pragma once
#include <tuple>
#include <type_traits>

#include "cpu.h"

// Sequential reader for arguments, either from registers+stack (a call or a
// "..." list) or from memory (a va_list).
struct ArgReader {
    Cpu* c;
    int ncrn;  // next core register
    u32 nsaa;  // next stacked argument address

    ArgReader(Cpu& cpu, int first_reg = 0) : c(&cpu), ncrn(first_reg), nsaa(cpu.sp()) {}
    static ArgReader from_va_list(Cpu& cpu, u32 ap) {
        ArgReader r(cpu, 4);
        r.nsaa = ap;
        return r;
    }

    u32 w32() {
        if (ncrn < 4) return c->r(ncrn++);
        u32 v = mem::rd32(nsaa);
        nsaa += 4;
        return v;
    }
    u64 w64() {
        if (ncrn < 4) {
            ncrn = (ncrn + 1) & ~1;
            if (ncrn <= 2) {
                u64 v = (u64)c->r(ncrn) | ((u64)c->r(ncrn + 1) << 32);
                ncrn += 2;
                return v;
            }
            ncrn = 4;
        }
        nsaa = align_up(nsaa, 8);
        u64 v = mem::ref<u64>(nsaa);
        nsaa += 8;
        return v;
    }

    template <class T> T get() {
        if constexpr (std::is_pointer_v<T>) return (T)mem::H(w32());
        else if constexpr (std::is_same_v<T, float>) return bitcast<float>(w32());
        else if constexpr (std::is_same_v<T, double>) return bitcast<double>(w64());
        else if constexpr (std::is_same_v<T, bool>) return w32() != 0;
        else if constexpr (sizeof(T) == 8) return (T)w64();
        else return (T)w32();
    }
};

template <class R> inline void put_ret(Cpu& c, R v) {
    if constexpr (std::is_pointer_v<R>) c.r(0) = mem::G((const void*)v);
    else if constexpr (std::is_same_v<R, float>) c.r(0) = bitcast<u32>(v);
    else if constexpr (std::is_same_v<R, double>) c.ret64(bitcast<u64>(v));
    else if constexpr (std::is_same_v<R, bool>) c.r(0) = v ? 1 : 0;
    else if constexpr (sizeof(R) == 8) c.ret64((u64)v);
    else c.r(0) = (u32)v;
}

template <class R, class... A> inline void invoke_host(R (*f)(A...), Cpu& c) {
    ArgReader rd(c);
    std::tuple<A...> args{rd.get<A>()...};  // braced init: left-to-right
    if constexpr (std::is_void_v<R>) std::apply(f, args);
    else put_ret(c, std::apply(f, args));
}

// W<fn> adapts a host function to a guest-callable HleFn.
template <auto F> void W(Cpu& c) { invoke_host(F, c); }
