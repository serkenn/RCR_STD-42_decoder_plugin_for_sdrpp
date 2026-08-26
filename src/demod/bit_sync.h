#pragma once
// Second-order zero-crossing DPLL symbol synchroniser for NRZ 2-FSK.
//
// The loop keeps a phase accumulator that advances by one symbol period per
// symbol.  Phase 0 is a symbol boundary, phase 0.5 the ideal sampling instant.
// Every zero crossing of the centred discriminator output is a boundary
// observation: its fractional position is interpolated between the two
// straddling samples, and the resulting timing error steers both the phase
// (alpha) and the rate estimate (beta).  Tracking the rate as well as the
// phase absorbs the transmitter/receiver clock difference over the long
// message batches STD-42 sends.

#include "demod/types.h"

#include <cmath>

namespace std42::demod {

class BitSync {
public:
    void configure(double sample_rate, double symbol_rate) {
        nominal_ = symbol_rate / sample_rate;      // symbols per sample
        // Allow ±2 % clock error, comfortably more than any real transmitter.
        freq_min_ = nominal_ * 0.98;
        freq_max_ = nominal_ * 1.02;
        reset();
    }

    void reset() {
        freq_ = nominal_;
        phase_ = 0.0;
        prev_ = 0.0f;
        armed_ = true;
        timing_err_ = 0.0;
    }

    // Feeds one centred sample. Returns true (and writes `out`) at each
    // mid-symbol sampling instant.
    bool process(float x, float& out) {
        bool emitted = false;

        const double p0 = phase_;
        phase_ += freq_;

        // Mid-symbol sample, linearly interpolated at phase == 0.5.
        if (armed_ && p0 < 0.5 && phase_ >= 0.5) {
            const double frac = (0.5 - p0) / freq_;      // in [0,1)
            out = prev_ + (x - prev_) * static_cast<float>(frac);
            armed_ = false;
            emitted = true;
        }

        // Timing error from the zero crossing, if there is one.
        if ((prev_ < 0.0f) != (x < 0.0f) && prev_ != x) {
            const double frac =
                static_cast<double>(prev_) / (static_cast<double>(prev_) - x);
            double e = p0 + freq_ * frac;                // phase at the crossing
            e -= std::floor(e);                          // wrap to [0,1)
            if (e >= 0.5) e -= 1.0;                      // wrap to [-0.5,0.5)

            phase_ -= kAlpha * e;
            freq_ = clampd(freq_ - kBeta * e, freq_min_, freq_max_);
            timing_err_ += (std::fabs(e) - timing_err_) * 0.02;
        }

        // Wrap; a new symbol period re-arms the sampler.
        if (phase_ >= 1.0) { phase_ -= 1.0; armed_ = true; }
        else if (phase_ < 0.0) { phase_ += 1.0; }

        prev_ = x;
        return emitted;
    }

    // Phase within the current symbol, 0..1 — drives the eye diagram.
    double phase() const { return phase_; }
    // Recovered symbol rate in Bd, given the sample rate used to configure.
    double symbol_rate(double sample_rate) const { return freq_ * sample_rate; }
    // Smoothed |timing error| in symbol periods; 0 is a perfectly locked loop.
    double timing_error() const { return timing_err_; }

private:
    // Critically-damped-ish loop: fast enough to pull in within the 576-bit
    // preamble, slow enough not to be dragged around by noise crossings.
    static constexpr double kAlpha = 0.10;
    static constexpr double kBeta  = 0.002;

    double nominal_ = 0.0;
    double freq_ = 0.0;
    double freq_min_ = 0.0;
    double freq_max_ = 0.0;
    double phase_ = 0.0;
    double timing_err_ = 0.0;
    float prev_ = 0.0f;
    bool armed_ = true;
};

} // namespace std42::demod
