#!/usr/bin/env bash
# Builds and runs the decoder self-test. Needs only a C++17 compiler — the
# tested layers (demod/, pocsag/) do not depend on SDR++.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/build/test_decoder"
mkdir -p "${ROOT}/build"

: "${CXX:=c++}"
"${CXX}" -std=c++17 -O2 -I"${ROOT}/src" \
    "${ROOT}/tests/test_decoder.cpp" \
    "${ROOT}/src/demod/fir.cpp" \
    "${ROOT}/src/demod/fsk_chain.cpp" \
    "${ROOT}/src/pocsag/bch.cpp" \
    "${ROOT}/src/pocsag/framer.cpp" \
    "${ROOT}/src/pocsag/interpret.cpp" \
    "${ROOT}/src/pocsag/message.cpp" \
    "${ROOT}/src/pocsag/receiver.cpp" \
    "${ROOT}/src/pocsag/sjis.cpp" \
    "${ROOT}/src/pocsag/sjis_table.cpp" \
    -o "${OUT}"

exec "${OUT}"
