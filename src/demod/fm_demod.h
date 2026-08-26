#pragma once
// Quadrature FM discriminator.
//
// arg(z[n] · conj(z[n-1])) is the phase advance per sample; scaling by
// fs/2π converts it to instantaneous frequency in Hz, which is the natural
// unit for STD-42: §2.1.6 puts the space tone at centre +4.5 kHz and the mark
// tone at centre -4.5 kHz, so a correctly tuned receiver sees ±4500.

#include "demod/types.h"

#include <cmath>

namespace std42::demod {

class FmDemod {
public:
    void configure(double sample_rate) {
        scale_ = static_cast<float>(sample_rate / (2.0 * M_PI));
        reset();
    }

    void reset() { prev_ = Complex32(0.0f, 0.0f); }

    // Returns instantaneous frequency deviation in Hz.
    float process(Complex32 s) {
        const Complex32 d = s * std::conj(prev_);
        prev_ = s;
        const float mag = std::abs(d);
        if (mag < 1e-20f) return 0.0f;
        return std::atan2(d.imag(), d.real()) * scale_;
    }

private:
    Complex32 prev_{0.0f, 0.0f};
    float scale_ = 1.0f;
};

} // namespace std42::demod
