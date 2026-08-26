#pragma once
// Eye diagram of the slicer input.
//
// The DPLL reports where each sample fell within the symbol period, so the
// samples can be folded back over a two-symbol window and accumulated into a
// decaying 2-D histogram — the classic eye pattern a paging monitor shows for
// tuning. A wide-open eye at the sampling instant (the dashed verticals) means
// the demodulator has margin; a closed one means it is guessing.

#include <functional>
#include <vector>

#include "demod/fsk_chain.h"

namespace std42::ui {

// Newest-first pull, matching demod::FskChain::pull_eye().
using EyePull = std::function<int(demod::EyePoint* dst, int capacity)>;

class EyeView {
public:
    static constexpr int kBinsX = 96;      // over two symbol periods
    static constexpr int kBinsY = 64;
    static constexpr float kValueMax = 1.8f;   // normalised deviation
    static constexpr float kDecay = 0.86f;
    static constexpr int kPullPerFrame = 1024;

    explicit EyeView(EyePull pull);

    void draw(float width, float height);
    void reset();

    // Separation between the two symbol clusters at the sampling instant,
    // normalised by their spread. Above ~3 the eye is comfortably open.
    double opening() const { return opening_; }

private:
    void accumulate(int n);

    EyePull pull_;
    std::vector<demod::EyePoint> scratch_;
    float accum_[kBinsX * kBinsY] = {};
    float peak_ = 1.0f;
    double opening_ = 0.0;
};

} // namespace std42::ui
