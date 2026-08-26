cmake_minimum_required(VERSION 3.13)
project(rcr_std42_decoder CXX)

# ------------------------------------------------------------------------------
# Out-of-tree SDR++ decoder module for RCR STD-42 — the ARIB/RCR standard for
# 280 MHz-band POCSAG paging (512 / 1200 bps, 2-FSK, ±4.5 kHz) that Japanese
# municipal 同報系 disaster-broadcast systems are built on.
#
# The plugin only consumes SDR++ headers; sdrpp_core symbols are resolved at
# runtime when SDR++ loads the shared library, so building needs a C++17
# compiler and the SDR++ source tree, not a built SDR++.
#
# The SDR++ source is searched, in order:
#   1. -DSDRPP_SOURCE_DIR=...
#   2. ./external/SDRPlusPlus   (populated by scripts/fetch-sdrpp.sh)
#   3. ../SDRPlusPlus           (sibling checkout)
# ------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Locate the SDR++ source tree ──────────────────────────────────────────────
set(_sdrpp_candidates "")
if (SDRPP_SOURCE_DIR)
    list(APPEND _sdrpp_candidates "${SDRPP_SOURCE_DIR}")
endif ()
list(APPEND _sdrpp_candidates
    "${CMAKE_CURRENT_SOURCE_DIR}/external/SDRPlusPlus"
    "${CMAKE_CURRENT_SOURCE_DIR}/../SDRPlusPlus")

set(SDRPP_SOURCE_DIR "")
foreach (_candidate IN LISTS _sdrpp_candidates)
    get_filename_component(_abs "${_candidate}" ABSOLUTE)
    if (EXISTS "${_abs}/core/src/module.h")
        set(SDRPP_SOURCE_DIR "${_abs}")
        break()
    endif ()
endforeach ()

if (NOT SDRPP_SOURCE_DIR)
    message(FATAL_ERROR
        "Could not locate the SDR++ source tree. Run scripts/fetch-sdrpp.sh "
        "to download it, or pass -DSDRPP_SOURCE_DIR=/path/to/SDRPlusPlus.")
endif ()
message(STATUS "Using SDR++ source tree: ${SDRPP_SOURCE_DIR}")

# ── Compiler flags — mirror SDR++'s own so the ABI matches sdrpp_core ─────────
if (CMAKE_BUILD_TYPE MATCHES "Debug")
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_MODULE_COMPILER_FLAGS -g -Og -std=c++17 -Wno-unused-command-line-argument)
    else ()
        set(SDRPP_MODULE_COMPILER_FLAGS -g -Og -std=c++17)
    endif ()
else ()
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(SDRPP_MODULE_COMPILER_FLAGS -O3 -std=c++17 -Wno-unused-command-line-argument)
    else ()
        set(SDRPP_MODULE_COMPILER_FLAGS -O3 -std=c++17)
    endif ()
endif ()

# CONFIGURE_DEPENDS re-runs the glob on every build, so a newly added source
# file is picked up without a manual reconfigure. The post-build check below is
# the backstop for the case where even that fails.
file(GLOB_RECURSE SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(${PROJECT_NAME} SHARED ${SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
target_compile_options(${PROJECT_NAME} PRIVATE ${SDRPP_MODULE_COMPILER_FLAGS})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SDRPP_SOURCE_DIR}/core/src"
    "${SDRPP_SOURCE_DIR}/core/src/imgui"
    "${SDRPP_SOURCE_DIR}/core/backends/glfw"
    "${SDRPP_SOURCE_DIR}/core/backends/glfw/imgui"
    "src/")

# SDR++ core headers include <volk/volk.h>, <fftw3.h> and <GLFW/glfw3.h>. They
# are only ever included, never linked — sdrpp_core resolves at load time — so
# the headers alone are enough. On macOS they come from Homebrew, which is not
# on the default search path.
if (APPLE)
    execute_process(COMMAND brew --prefix
                    OUTPUT_VARIABLE HOMEBREW_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if (HOMEBREW_PREFIX AND EXISTS "${HOMEBREW_PREFIX}/include")
        message(STATUS "Using Homebrew headers from ${HOMEBREW_PREFIX}/include")
        target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
            "${HOMEBREW_PREFIX}/include")
    endif ()
endif ()

# dlsym() for the ImGui backend entry points used to re-upload the font atlas.
target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# sdrpp_core symbols are resolved when SDR++ dlopen()s the module, so they must
# be allowed to stay unresolved at link time.
#   * Mach-O (macOS): -undefined dynamic_lookup
#   * ELF (Linux):    --unresolved-symbols=ignore-in-object-files
if (APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE -undefined dynamic_lookup)
    # SDR++ scans its plugin directory for SDRPP_MOD_EXTENTSION, which is
    # ".dylib" on macOS and ".so" elsewhere (core/src/module.h).
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".dylib")

    # A universal binary covers both Apple Silicon and Intel from one artefact.
    # Nothing architecture-specific is linked — the SDR++ headers we compile
    # against are arch-independent and sdrpp_core resolves at load time.
    if (NOT CMAKE_OSX_ARCHITECTURES)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            OSX_ARCHITECTURES "arm64;x86_64")
    endif ()
else ()
    target_link_options(${PROJECT_NAME} PRIVATE
        -Wl,--unresolved-symbols=ignore-in-object-files)
endif ()

# The permissive link flags above let sdrpp_core symbols stay unresolved — and
# would just as happily let a missing source file through, which shows up only
# as a jump to address 0 once SDR++ loads the module. Verify after every link
# that the library defines everything in its own namespace.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-undefined.sh"
            "$<TARGET_FILE:${PROJECT_NAME}>"
    VERBATIM
    COMMENT "Checking that no std42:: symbol was left undefined")

install(TARGETS ${PROJECT_NAME} DESTINATION lib/sdrpp/plugins)
