#pragma once
// A one-line metric row: caption on the left, a rolling bar history in the
// middle, the current value on the right. The panel pushes one sample every
// 0.5 s, so the 120-slot ring is a 60-second window.

#include <imgui.h>

#include <limits>
#include <string>

namespace std42::ui {

class Sparkline {
public:
    static constexpr int kCapacity = 120;

    void push(double v) {
        ring_[write_] = v;
        write_ = (write_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
    }

    void clear() { write_ = 0; count_ = 0; }

    // Pins the vertical scale. Either bound may be left NaN to auto-scale.
    void set_range(double lo, double hi) { lo_ = lo; hi_ = hi; }

    void draw(const char* caption,
              const std::string& value_text,
              ImU32 color,
              float caption_w,
              float row_h = 18.0f) const;

private:
    double ring_[kCapacity] = {};
    int write_ = 0;
    int count_ = 0;
    double lo_ = std::numeric_limits<double>::quiet_NaN();
    double hi_ = std::numeric_limits<double>::quiet_NaN();
};

} // namespace std42::ui
