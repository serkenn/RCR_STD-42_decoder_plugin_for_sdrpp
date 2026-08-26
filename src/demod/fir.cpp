#include "demod/fir.h"

#include <cmath>

namespace std42::demod {

std::vector<float> design_lowpass(double sample_rate, double cutoff_hz, int taps) {
    if (taps < 3) taps = 3;
    if ((taps & 1) == 0) ++taps;               // keep it odd / linear phase

    const double fc = cutoff_hz / sample_rate; // normalised (cycles/sample)
    const int mid = taps / 2;

    std::vector<float> h(static_cast<size_t>(taps));
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const int n = i - mid;
        // Ideal low-pass impulse response, 2*fc*sinc(2*fc*n).
        const double sinc = (n == 0) ? 2.0 * fc
                                     : std::sin(2.0 * M_PI * fc * n) / (M_PI * n);
        // Hamming window.
        const double w = 0.54 - 0.46 * std::cos(2.0 * M_PI * i / (taps - 1));
        const double v = sinc * w;
        h[static_cast<size_t>(i)] = static_cast<float>(v);
        sum += v;
    }
    // Unity DC gain.
    if (sum != 0.0) {
        for (auto& v : h) v = static_cast<float>(v / sum);
    }
    return h;
}

void FirFilter::set_taps(std::vector<float> taps) {
    taps_ = std::move(taps);
    hist_.assign(taps_.size(), 0.0f);
    pos_ = 0;
}

void FirFilter::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    pos_ = 0;
}

float FirFilter::process(float x) {
    const size_t n = taps_.size();
    if (n == 0) return x;

    hist_[pos_] = x;

    double acc = 0.0;
    size_t idx = pos_;
    for (size_t i = 0; i < n; ++i) {
        acc += static_cast<double>(taps_[i]) * hist_[idx];
        idx = (idx == 0) ? n - 1 : idx - 1;
    }

    pos_ = (pos_ + 1) % n;
    return static_cast<float>(acc);
}

} // namespace std42::demod
