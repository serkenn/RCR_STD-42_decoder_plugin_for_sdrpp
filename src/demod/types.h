#pragma once
// Small numeric helpers shared by the DSP blocks.

#include <complex>

namespace std42::demod {

using Complex32 = std::complex<float>;

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace std42::demod
