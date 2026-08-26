#!/usr/bin/env bash
# Fail the build if the plugin leaves any of its OWN symbols undefined.
#
# The module is linked with -undefined dynamic_lookup (Mach-O) or
# --unresolved-symbols=ignore-in-object-files (ELF) so that sdrpp_core symbols
# can be resolved when SDR++ dlopen()s it. That is necessary, but it also means
# a source file missing from the build links cleanly and only fails at runtime
# — on macOS a missing dynamic_lookup symbol binds to NULL, so the first call
# jumps to address 0 and segfaults inside SDR++.
#
# Everything under the std42:: namespace must be defined by this library, so an
# undefined one is always a build error. Mangled as _ZN5std42… on ELF and
# __ZN5std42… on Mach-O; the pattern below matches both.
set -euo pipefail

LIB="${1:?usage: check-undefined.sh <shared-library>}"

if ! command -v nm >/dev/null 2>&1; then
    echo "check-undefined: nm not found, skipping" >&2
    exit 0
fi

if [ ! -f "${LIB}" ]; then
    echo "check-undefined: ${LIB} does not exist" >&2
    exit 1
fi

# A failure of nm itself must not read as a clean bill of health.
if ! symbols="$(nm -u "${LIB}" 2>&1)"; then
    echo "check-undefined: nm failed on ${LIB}:" >&2
    echo "${symbols}" | sed 's/^/    /' >&2
    exit 1
fi

missing="$(printf '%s\n' "${symbols}" | awk '{print $NF}' | grep -E '_ZN5std42|_ZNK5std42' || true)"

if [ -n "${missing}" ]; then
    echo "" >&2
    echo "ERROR: ${LIB} leaves its own symbols undefined:" >&2
    echo "" >&2
    if command -v c++filt >/dev/null 2>&1; then
        echo "${missing}" | c++filt | sed 's/^/    /' >&2
    else
        echo "${missing}" | sed 's/^/    /' >&2
    fi
    echo "" >&2
    echo "A source file is almost certainly missing from the build. The glob in" >&2
    echo "CMakeLists.txt is evaluated at configure time, so re-run cmake:" >&2
    echo "    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release" >&2
    echo "" >&2
    exit 1
fi

echo "check-undefined: ${LIB} defines all std42:: symbols"
