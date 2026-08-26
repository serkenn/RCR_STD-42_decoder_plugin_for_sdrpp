#include "demod/fsk_chain.h"

#include <algorithm>
#include <cmath>

namespace std42::demod {

void FskChain::configure(double sample_rate, double baud) {
    sample_rate_ = sample_rate;
    baud_ = baud;

    // Cutoff a little above the symbol rate keeps the NRZ main lobe intact
    // while rejecting the wideband click noise the discriminator produces on
    // noise-only input. Tap count scales with samples-per-symbol so the
    // transition width stays a fixed fraction of the symbol rate.
    const double cutoff = baud * 1.1;
    const int taps = std::clamp(
        static_cast<int>(std::lround(sample_rate / baud * 2.0)) | 1, 15, 255);
    lpf_.set_taps(design_lowpass(sample_rate, cutoff, taps));

    level_.configure(sample_rate, baud);
    sync_.configure(sample_rate, baud);

    eye_.assign(kEyeCapacity, EyePoint{0.0f, 0.0f});
    eye_write_.store(0, std::memory_order_relaxed);
    symbols_.store(0, std::memory_order_relaxed);
}

void FskChain::reset() {
    lpf_.reset();
    level_.reset();
    sync_.reset();
    symbols_.store(0, std::memory_order_relaxed);
}

int FskChain::process(float freq_hz) {
    const float filtered = lpf_.process(freq_hz);
    const float centred = level_.process(filtered);

    // Normalise by the tracked deviation so the eye and the slicer are
    // independent of signal strength and of the actual deviation used.
    const float dev = level_.deviation();
    const float norm = (dev > 1.0f) ? centred / dev : 0.0f;

    const int w = eye_write_.load(std::memory_order_relaxed);
    eye_[static_cast<size_t>(w)] =
        EyePoint{static_cast<float>(sync_.phase()), clampf(norm, -2.0f, 2.0f)};
    eye_write_.store((w + 1) % kEyeCapacity, std::memory_order_release);

    // Require a third of the tracked deviation of excursion before believing a
    // zero crossing, so inter-burst noise does not drive the timing loop.
    float sym = 0.0f;
    if (!sync_.process(centred, dev * 0.33f, sym)) return -1;

    symbols_.fetch_add(1, std::memory_order_relaxed);

    // STD-42 §2.1.6: the space tone sits at centre +4.5 kHz and the mark tone
    // at centre -4.5 kHz. In the telegraphy convention mark is binary 1, so a
    // positive discriminator output is a binary 0. The framer additionally
    // resolves polarity from the sync codeword, which covers receivers or
    // front-ends that invert the spectrum.
    return (sym < 0.0f) ? 1 : 0;
}

int FskChain::pull_eye(EyePoint* dst, int capacity) const {
    if (eye_.empty() || capacity <= 0) return 0;
    const int w = eye_write_.load(std::memory_order_acquire);
    const int n = std::min(capacity, kEyeCapacity);
    for (int i = 0; i < n; ++i) {
        int idx = w - 1 - i;
        if (idx < 0) idx += kEyeCapacity;
        dst[i] = eye_[static_cast<size_t>(idx)];
    }
    return n;
}

} // namespace std42::demod
