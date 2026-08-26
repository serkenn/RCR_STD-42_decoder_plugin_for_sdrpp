#include "pocsag/bch.h"

#include <mutex>

namespace std42::pocsag {

namespace {

// Syndrome → error mask, in codeword bit positions (bit 31 == spec bit 1).
uint32_t g_error_table[1024];
std::once_flag g_once;

inline int popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(v);
#else
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
#endif
}

// Even parity over all 32 bits.
inline int parity32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_parity(v);
#else
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return static_cast<int>(v & 1u);
#endif
}

void build_table() {
    for (auto& e : g_error_table) e = 0;

    // Double-bit patterns first, then single-bit ones. The code's minimum
    // distance of 5 guarantees the two sets have disjoint syndromes, so the
    // ordering only matters as a belt-and-braces measure.
    for (int i = 0; i < 31; ++i) {
        const uint32_t mi = 1u << (i + 1);
        for (int j = i + 1; j < 31; ++j) {
            const uint32_t m = mi | (1u << (j + 1));
            g_error_table[bch_syndrome(m)] = m;
        }
    }
    for (int i = 0; i < 31; ++i) {
        const uint32_t m = 1u << (i + 1);
        g_error_table[bch_syndrome(m)] = m;
    }
    // Syndrome 0 means "no error", never an error pattern.
    g_error_table[0] = 0;
}

} // namespace

void bch_init() { std::call_once(g_once, build_table); }

uint32_t bch_syndrome(uint32_t cw) {
    // Drop the parity bit; the remaining 31 bits are the BCH codeword with
    // bit 30 the highest-order coefficient.
    uint32_t s = cw >> 1;
    for (int i = 30; i >= 10; --i) {
        if (s & (1u << i)) s ^= kBchGenerator << (i - 10);
    }
    return s & 0x3FFu;
}

int bch_correct(uint32_t& cw) {
    bch_init();

    int corrected = 0;
    const uint32_t s = bch_syndrome(cw);
    if (s != 0) {
        const uint32_t e = g_error_table[s];
        if (e == 0) return -1;              // syndrome outside the 1-2 bit set
        cw ^= e;
        corrected = popcount32(e);
    }

    // The parity bit is not covered by the BCH syndrome, so a parity mismatch
    // now means the parity bit itself was received wrong. That still counts
    // against the two-error budget.
    if (parity32(cw) != 0) {
        cw ^= 1u;
        ++corrected;
        if (corrected > 2) return -1;
    }
    return corrected;
}

} // namespace std42::pocsag
