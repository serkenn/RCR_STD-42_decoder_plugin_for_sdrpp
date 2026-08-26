#pragma once
// Baseline (DC) and deviation tracking for the discriminator output.
//
// Two things have to be estimated before the signal can be sliced:
//
//   * Centre.  A carrier offset shows up as a constant frequency error at the
//     discriminator output.  STD-42 §2.1.8 deliberately offsets co-channel
//     base stations by up to ±1.25 kHz, and the user's tuning adds more, so
//     the centre cannot be assumed to be 0 Hz.  POCSAG is very nearly DC-free
//     over a batch (the preamble is 1010…, and both the sync codeword
//     0x7CD215D8 and the idle codeword 0x7A89C197 carry exactly 16 ones), so a
//     slow mean is an unbiased centre estimate.
//
//   * Deviation.  E[|x - centre|] of an ideal 2-FSK NRZ stream is the peak
//     deviation, i.e. ~4500 Hz for a correctly received signal.  It normalises
//     the slicer input and doubles as the "is there a signal at all" metric.

#include <cmath>

namespace std42::demod {

class LevelTracker {
public:
    // `symbol_rate` sets the loop time constants in symbols, so the tracker
    // behaves the same at 512 and 1200 bps.
    void configure(double sample_rate, double symbol_rate) {
        const double sps = sample_rate / symbol_rate;
        // ~64 symbols for the centre, ~32 for the amplitude.
        centre_k_ = 1.0f / static_cast<float>(sps * 64.0);
        amp_k_    = 1.0f / static_cast<float>(sps * 32.0);
        reset();
    }

    void reset() {
        centre_ = 0.0f;
        amp_ = 0.0f;
    }

    // Feeds one discriminator sample and returns it centred.
    float process(float x) {
        centre_ += (x - centre_) * centre_k_;
        const float centred = x - centre_;
        amp_ += (std::fabs(centred) - amp_) * amp_k_;
        return centred;
    }

    float centre() const { return centre_; }   // Hz — carrier offset estimate
    float deviation() const { return amp_; }   // Hz — mean |deviation|

private:
    float centre_ = 0.0f;
    float amp_ = 0.0f;
    float centre_k_ = 1e-4f;
    float amp_k_ = 1e-4f;
};

} // namespace std42::demod
