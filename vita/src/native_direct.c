// Imports whose bionic and Vita (newlib/libgcc) versions have the same ABI:
// integer/pointer arguments only, same struct layouts. The game calls these
// natively instead of through a thunk, which matters for memcpy & co.
// The __aeabi_* helpers use the base (softfp) calling convention on every
// ARM target, so they qualify too. Anything taking or returning a float, or
// touching FILE/stat/ctype, must stay in the HLE tables.
#include <ctype.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "native_direct.h"

extern int __aeabi_idiv(int, int);
extern unsigned __aeabi_uidiv(unsigned, unsigned);
extern void __aeabi_idivmod(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_uldivmod(void);
extern void __aeabi_d2iz(void);
extern void __aeabi_d2lz(void);
extern void __aeabi_dadd(void);
extern void __aeabi_ddiv(void);
extern void __aeabi_dmul(void);
extern void __aeabi_i2d(void);
extern void __aeabi_ul2d(void);
extern int __cxa_guard_acquire(int*);
extern void __cxa_guard_release(int*);

const NativeDirect native_direct[] = {
    {"memcpy", (void*)memcpy}, {"memmove", (void*)memmove}, {"memset", (void*)memset},
    {"memcmp", (void*)memcmp}, {"memchr", (void*)memchr}, {"strlen", (void*)strlen},
    {"strcmp", (void*)strcmp}, {"strncmp", (void*)strncmp}, {"strcpy", (void*)strcpy},
    {"strncpy", (void*)strncpy}, {"strcat", (void*)strcat}, {"strncat", (void*)strncat},
    {"strchr", (void*)strchr}, {"strrchr", (void*)strrchr}, {"strstr", (void*)strstr},
    {"strcasecmp", (void*)strcasecmp}, {"strncasecmp", (void*)strncasecmp},
    {"atoi", (void*)atoi}, {"atol", (void*)atol}, {"strtol", (void*)strtol}, {"toupper", (void*)toupper},
    {"malloc", (void*)malloc}, {"free", (void*)free}, {"calloc", (void*)calloc}, {"realloc", (void*)realloc},
    {"_Znwj", (void*)malloc}, {"_Znaj", (void*)malloc}, {"_ZnajRKSt9nothrow_t", (void*)malloc},
    {"_ZdlPv", (void*)free}, {"_ZdaPv", (void*)free},
    {"__aeabi_idiv", (void*)__aeabi_idiv}, {"__aeabi_uidiv", (void*)__aeabi_uidiv},
    {"__aeabi_idivmod", (void*)__aeabi_idivmod}, {"__aeabi_uidivmod", (void*)__aeabi_uidivmod},
    {"__aeabi_ldivmod", (void*)__aeabi_ldivmod}, {"__aeabi_uldivmod", (void*)__aeabi_uldivmod},
    {"__aeabi_d2iz", (void*)__aeabi_d2iz}, {"__aeabi_d2lz", (void*)__aeabi_d2lz},
    {"__aeabi_dadd", (void*)__aeabi_dadd}, {"__aeabi_ddiv", (void*)__aeabi_ddiv},
    {"__aeabi_dmul", (void*)__aeabi_dmul}, {"__aeabi_i2d", (void*)__aeabi_i2d},
    {"__aeabi_ul2d", (void*)__aeabi_ul2d},
    {"__cxa_guard_acquire", (void*)__cxa_guard_acquire}, {"__cxa_guard_release", (void*)__cxa_guard_release},
    // bionic's jmp_buf (256 bytes) is bigger than newlib's, so the native pair fits
    {"setjmp", (void*)setjmp}, {"longjmp", (void*)longjmp},
    {0, 0},
};
