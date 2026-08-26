#pragma once
// Recognises a steady alternating bit pattern on the channel.
//
// Municipal STD-42 installations are not silent between calls: the Karatsu
// 283.5365 MHz transmitter, for instance, radiates a continuous 1010… idle
// pattern at 750 bps with ±5 kHz deviation, and only interrupts it for a ~1 s
// 1200 bps POCSAG burst every 14 minutes. Without knowing that, the panel's
// "signal present, no frame sync" reads like a fault for 99.7 % of the time
// when in fact the receiver is healthy and the channel is simply idle.
//
// A strict 1010… pattern puts a zero crossing at every bit boundary, so the
// crossing interval is both short and very regular; data is neither. Tracking
// the mean and spread of that interval identifies the pattern and measures its
// rate, without needing an FFT on the DSP thread.

#include "demod/fir.h"

#include <cmath>

namespace std42::demod {

class PatternDetector {
public:
    void configure(double sample_rate) {
        sample_rate_ = sample_rate;
        // The raw discriminator output carries the wideband click noise that
        // FM produces off-signal, which manufactures zero crossings far faster
        // than any bit pattern. Band-limiting first is what makes the crossing
        // interval meaningful: 2 kHz passes both the 375 Hz fundamental of a
        // 750 bps idle pattern and the 600 Hz of a 1200 bps POCSAG preamble,
        // while removing everything above them.
        lpf_.set_taps(design_lowpass(sample_rate, 2000.0, 63));
        reset();
    }

    void reset() {
        lpf_.reset();
        centre_ = 0.0f;
        amp_ = 0.0f;
        prev_ = 0.0f;
        since_ = 0;
        median_ = 0.0;
        regular_ = 0.0;
        have_ = false;
    }

    // Feeds one raw discriminator sample, in Hz.
    void process(float raw_hz) {
        const float hz = lpf_.process(raw_hz);
        // Slow centre estimate; the pattern itself is DC-free.
        centre_ += (hz - centre_) * kCentreK;
        const float x = hz - centre_;
        amp_ += (std::fabs(x) - amp_) * kAmpK;

        ++since_;
        // Hysteresis keeps noise crossings out of the statistics.
        const float h = amp_ * 0.4f;
        if (x > h) swung_high_ = true;
        if (x < -h) swung_low_ = true;

        if ((prev_ < 0.0f) != (x < 0.0f)) {
            const bool rising = (x >= 0.0f);
            if (rising ? swung_low_ : swung_high_) {
                if (rising) swung_low_ = false; else swung_high_ = false;
                const double interval = static_cast<double>(since_);
                since_ = 0;
                if (!have_) {
                    median_ = interval;
                    have_ = true;
                } else {
                    // A median, not a mean. Measured against the Karatsu idle
                    // carrier the crossing intervals cluster hard on 64 samples
                    // (= 750.0 bps at 48 kHz) but carry a tail of shorter ones
                    // from noise; the mean of that distribution reads 834 bps,
                    // the median reads 750.0. A sign-driven update converges to
                    // the median and simply ignores how far out an outlier is.
                    const double step = std::max(0.02, median_ * 0.004);
                    median_ += (interval > median_) ? step : -step;

                    // Regularity is the share of intervals that land near the
                    // median — a metronomic 1010… pattern scores high, data
                    // (whose runs are multiples of the bit period) does not.
                    const bool close =
                        std::fabs(interval - median_) < median_ * 0.12;
                    regular_ += ((close ? 1.0 : 0.0) - regular_) * kStatK;
                }
            }
        }
        prev_ = x;
    }

    // Bit rate implied by the crossing interval, if every bit is a transition.
    double pattern_rate() const {
        if (!have_ || median_ < 1.0) return 0.0;
        return sample_rate_ / median_;
    }

    // 0..1; near 1 when the crossings are metronomic, i.e. a 1010… pattern.
    double regularity() const { return have_ ? regular_ : 0.0; }

    float deviation() const { return amp_; }

    // A confident idle-pattern call: regular crossings and real deviation.
    bool is_idle_pattern() const {
        return regularity() > 0.55 && amp_ > 1500.0f && pattern_rate() > 100.0;
    }

private:
    static constexpr float kCentreK = 2e-5f;
    static constexpr float kAmpK = 5e-5f;
    static constexpr double kStatK = 0.01;

    FirFilter lpf_;
    double sample_rate_ = 48000.0;
    float centre_ = 0.0f, amp_ = 0.0f, prev_ = 0.0f;
    bool swung_high_ = false, swung_low_ = false;
    long long since_ = 0;
    double median_ = 0.0, regular_ = 0.0;
    bool have_ = false;
};

} // namespace std42::demod
