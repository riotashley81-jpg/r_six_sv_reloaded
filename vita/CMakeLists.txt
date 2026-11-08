cmake_minimum_required(VERSION 3.16)

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Set VITASDK to your VitaSDK install (e.g. /usr/local/vitasdk)")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(r6sv_vita C CXX ASM)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Rainbow Six: Shadow Vanguard")
set(VITA_TITLEID  "RSIX00001")
set(VITA_VERSION  "00.10")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-O2 -Wall -Wno-unused-function)

# The loader core is shared with the PC build (../src); only the CPU, memory,
# GL and frontend layers are Vita-specific.
set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(r6sv
    ${SHARED}/elf.cpp
    ${SHARED}/hle.cpp
    ${SHARED}/hle_libc.cpp
    ${SHARED}/hle_stdio.cpp
    ${SHARED}/hle_pthread.cpp
    ${SHARED}/hle_net.cpp
    ${SHARED}/jni.cpp
    ${SHARED}/java_classes.cpp
    ${SHARED}/texture_decode.cpp
    ${SHARED}/ffp_fix.cpp
    ${SHARED}/glsl_to_cg.cpp
    src/cpu_vita.cpp
    src/mem_vita.cpp
    src/hle_gl_vita.cpp
    src/native_direct.c
    src/thunks.S
    src/main_vita.cpp)
target_include_directories(r6sv PRIVATE ${SHARED} src)
target_link_libraries(r6sv
    vitaGL vitashark SceShaccCgExt mathneon taihen_stub kubridge_stub
    SceShaccCg_stub SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCtrl_stub
    SceTouch_stub SceAppMgr_stub SceAppUtil_stub SceCommonDialog_stub SceSysmodule_stub
    SceAudio_stub SceIme_stub ScePower_stub SceLibKernel_stub SceKernelThreadMgr_stub
    stdc++ m
    # libstdc++/libgcc only treat the program as threaded if pthread_cancel is
    # linked (a weak reference doesn't pull it in); otherwise std::mutex is a
    # no-op and thread_local is one copy shared by all threads.
    -Wl,--whole-archive pthread -Wl,--no-whole-archive)

vita_create_self(eboot.bin r6sv UNSAFE)
# LiveArea images: made by make_livearea.py from the game's own art, so they
# are not in the repository. Without them the vpk gets the default bubble.
# Packaged from a copy in the build tree: vita-pack-vpk can't take paths with
# spaces.
set(LA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys)
set(LA_FILES icon0.png pic0.png livearea/contents/bg.png
    livearea/contents/startup.png livearea/contents/template.xml)
set(LA_ARGS)
if (EXISTS ${LA_SRC}/icon0.png)
    file(COPY ${LA_SRC} DESTINATION ${CMAKE_BINARY_DIR})
    foreach(f ${LA_FILES})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LA_SRC}/${f})
        list(APPEND LA_ARGS FILE ${CMAKE_BINARY_DIR}/sce_sys/${f} sce_sys/${f})
    endforeach()
else()
    message(STATUS "No LiveArea images in vita/sce_sys (run make_livearea.py); using the default bubble")
endif()
vita_create_vpk(r6sv.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    ${LA_ARGS})
