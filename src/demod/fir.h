#pragma once
// Windowed-sinc low-pass FIR with a real-valued delay line.
//
// Used as the post-discriminator baseband filter: the 2-FSK NRZ main lobe is
// roughly ±R_b wide, so a cutoff a little above the symbol rate keeps the eye
// open while removing the click noise the discriminator produces off-signal.

#include <cstddef>
#include <vector>

namespace std42::demod {

// Hamming-windowed sinc low-pass. `cutoff_hz` is the -6 dB point; `taps` is
// forced odd so the filter is linear phase with an integer group delay.
std::vector<float> design_lowpass(double sample_rate, double cutoff_hz, int taps);

class FirFilter {
public:
    FirFilter() = default;
    explicit FirFilter(std::vector<float> taps) { set_taps(std::move(taps)); }

    void set_taps(std::vector<float> taps);
    void reset();

    float process(float x);

    size_t size() const { return taps_.size(); }

private:
    std::vector<float> taps_;
    std::vector<float> hist_;   // circular delay line
    size_t pos_ = 0;
};

} // namespace std42::demod
