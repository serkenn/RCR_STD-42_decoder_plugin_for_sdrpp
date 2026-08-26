#pragma once
// One complete baud-specific 2-FSK slicing chain:
//
//   discriminator (Hz) → low-pass → centre/deviation tracking → symbol DPLL
//                      → hard decision → bits
//
// The chain is deliberately cheap so that "Auto" baud mode can run one of
// these per candidate rate (512 / 1200 / 2400 Bd) side by side and simply use
// whichever one achieves frame sync, instead of hunting one rate at a time.

#include "demod/bit_sync.h"
#include "demod/fir.h"
#include "demod/level_tracker.h"
#include "demod/types.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace std42::demod {

// One point of the eye diagram: the normalised slicer input against its
// position within the symbol period.
struct EyePoint {
    float phase;   // 0..1 within the symbol
    float value;   // normalised to roughly ±1
};

class FskChain {
public:
    static constexpr int kEyeCapacity = 4096;

    void configure(double sample_rate, double baud);
    void reset();

    // Feeds one discriminator sample (Hz). Returns 0 or 1 when a symbol
    // instant falls on this sample, otherwise -1.
    int process(float freq_hz);

    double baud() const { return baud_; }

    // ── Metrics (read from the UI thread) ─────────────────────────────────
    float carrier_offset() const { return level_.centre(); }        // Hz
    float deviation() const { return level_.deviation(); }          // Hz
    double recovered_baud() const { return sync_.symbol_rate(sample_rate_); }
    double timing_error() const { return sync_.timing_error(); }
    long long symbols() const { return symbols_.load(std::memory_order_relaxed); }

    // Newest-first snapshot of the eye ring buffer. Returns the count written.
    int pull_eye(EyePoint* dst, int capacity) const;

private:
    double sample_rate_ = 0.0;
    double baud_ = 0.0;

    FirFilter lpf_;
    LevelTracker level_;
    BitSync sync_;

    std::atomic<long long> symbols_{0};

    // Single-producer ring; the UI reads a possibly-torn tail, which is
    // harmless for a scatter plot.
    std::vector<EyePoint> eye_;
    std::atomic<int> eye_write_{0};
};

} // namespace std42::demod
