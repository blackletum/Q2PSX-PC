cmake_minimum_required(VERSION 3.20)
project(Q2PSX-PC
        VERSION 0.1.0
        DESCRIPTION "Native PC recreation of Quake II for PlayStation 1"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(Q2PSX_BUILD_TOOLS  "Build offline inspection tools"    ON)
option(Q2PSX_BUILD_CLIENT "Build the playable client (needs SDL3)" ON)
option(Q2PSX_WERROR      "Treat warnings as errors"           OFF)

# ---------------------------------------------------------------------------
# Warning flags
# ---------------------------------------------------------------------------
add_library(q2psx_flags INTERFACE)

if(MSVC)
    target_compile_options(q2psx_flags INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(q2psx_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
    if(Q2PSX_WERROR)
        target_compile_options(q2psx_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(q2psx_flags INTERFACE
        -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual
        -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter)
    if(Q2PSX_WERROR)
        target_compile_options(q2psx_flags INTERFACE -Werror)
    endif()
    # clang targeting the MSVC runtime still pulls in the UCRT's deprecation
    # attributes, which fire on every fopen/strncpy in portable code.
    if(WIN32)
        target_compile_definitions(q2psx_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# Signed integer overflow is well-defined two's-complement here: the GTE and the
# fixed-point math deliberately rely on wrapping to reproduce hardware saturation
# behaviour bit-for-bit.
if(NOT MSVC)
    target_compile_options(q2psx_flags INTERFACE -fwrapv)
endif()

# ---------------------------------------------------------------------------
# Core libraries
# ---------------------------------------------------------------------------
add_subdirectory(src/common)
add_subdirectory(src/disc)
add_subdirectory(src/build)
add_subdirectory(src/formats)
add_subdirectory(src/psx)
add_subdirectory(src/audio)
add_subdirectory(src/render)
add_subdirectory(src/game)

if(Q2PSX_BUILD_TOOLS)
    add_subdirectory(tools/q2psx-inspect)
endif()

# ---------------------------------------------------------------------------
# Client (optional — requires SDL3)
# ---------------------------------------------------------------------------
if(Q2PSX_BUILD_CLIENT)
    find_package(SDL3 QUIET CONFIG)
    if(SDL3_FOUND)
        message(STATUS "SDL3 found — building playable client")
        add_subdirectory(src/client)
    else()
        message(STATUS "SDL3 not found — skipping client (core libs and tools still build). "
                       "Set SDL3_DIR to enable it.")
    endif()
endif()

message(STATUS "")
message(STATUS "Q2PSX-PC ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  tools      : ${Q2PSX_BUILD_TOOLS}")
message(STATUS "")
