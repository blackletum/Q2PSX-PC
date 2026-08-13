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

# ---------------------------------------------------------------------------
# Version
#
# The version is declared once, in the project() call above. The generated
# header derives from it, so a binary cannot report a version that disagrees
# with the build system.
#
# Git state is captured when available. Outside a checkout the describe string
# falls back to the bare version rather than inventing a hash.
# ---------------------------------------------------------------------------
set(Q2PSX_GIT_DESCRIBE "v${PROJECT_VERSION}")
set(Q2PSX_GIT_DIRTY 0)

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty=-dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(GIT_DESC)
        set(Q2PSX_GIT_DESCRIBE "${GIT_DESC}")
        if(GIT_DESC MATCHES "-dirty$")
            set(Q2PSX_GIT_DIRTY 1)
        endif()
    endif()
endif()

configure_file(
    ${CMAKE_SOURCE_DIR}/src/common/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY)

option(Q2PSX_BUILD_TOOLS  "Build offline inspection tools"    ON)
option(Q2PSX_BUILD_CLIENT "Build the playable client (needs SDL3)" ON)
option(Q2PSX_FETCH_SDL3   "Fetch and build SDL3 if not installed"  OFF)
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

enable_testing()
add_subdirectory(tests)

if(Q2PSX_BUILD_TOOLS)
    add_subdirectory(tools/q2psx-inspect)
endif()

# ---------------------------------------------------------------------------
# Client (optional — requires SDL3)
# ---------------------------------------------------------------------------
if(Q2PSX_BUILD_CLIENT)
    find_package(SDL3 QUIET CONFIG)

    # No system SDL3? Optionally fetch and build it. This is off by default
    # because it turns a five-second configure into a multi-minute one, and the
    # core libraries and offline tools do not need it.
    if(NOT SDL3_FOUND AND Q2PSX_FETCH_SDL3)
        message(STATUS "SDL3 not found — fetching it (this takes a while)")
        include(FetchContent)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG        release-3.2.8
            GIT_SHALLOW    TRUE)
        set(SDL_SHARED ON  CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TEST   OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
        set(SDL3_FOUND TRUE)
    endif()

    if(SDL3_FOUND)
        message(STATUS "building the playable client")
        add_subdirectory(src/client)
    else()
        message(STATUS "SDL3 not found — skipping the client; core libs and tools still build.")
        message(STATUS "  configure with -DQ2PSX_FETCH_SDL3=ON to build SDL3 from source,")
        message(STATUS "  or set SDL3_DIR to an existing install.")
    endif()
endif()

message(STATUS "")
message(STATUS "Q2PSX-PC ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  tools      : ${Q2PSX_BUILD_TOOLS}")
message(STATUS "")
