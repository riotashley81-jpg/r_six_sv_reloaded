cmake_minimum_required(VERSION 3.20)
project(r6sv_loader CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- dynarmic (32-bit ARM JIT) ---
set(Boost_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/boost CACHE PATH "" FORCE)
set(BOOST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/external/boost CACHE PATH "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

# --- SDL2 (prebuilt VC package) ---
set(SDL2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/SDL2/cmake)
find_package(SDL2 REQUIRED CONFIG)

# --- loader ---
file(GLOB LOADER_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(r6sv ${LOADER_SOURCES})
target_link_libraries(r6sv PRIVATE dynarmic SDL2::SDL2 ws2_32)
add_custom_command(TARGET r6sv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:r6sv>)
target_compile_definitions(r6sv PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
if (MSVC)
    target_compile_options(r6sv PRIVATE /utf-8 /W3 /permissive-)
endif()
