#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NativeDirect {
    const char* name;
    void* fn;
} NativeDirect;

extern const NativeDirect native_direct[];  // terminated by {0, 0}

#ifdef __cplusplus
}
#endif
