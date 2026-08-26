#include "ui/sparkline.h"

#include <algorithm>
#include <cmath>

namespace std42::ui {

void Sparkline::draw(const char* caption,
                     const std::string& value_text,
                     ImU32 color,
                     float caption_w,
                     float row_h) const {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail <= caption_w + 24.0f) {
        // Too narrow for the history — degrade to "caption: value".
        ImGui::TextDisabled("%s", caption);
        ImGui::SameLine();
        ImGui::TextUnformatted(value_text.c_str());
        return;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    dl->AddText(origin, dim, caption);

    const ImVec2 value_sz = ImGui::CalcTextSize(value_text.c_str());
    const float value_x =
        std::max(origin.x + caption_w + 8.0f, origin.x + avail - value_sz.x);
    dl->AddText(ImVec2(value_x, origin.y), color, value_text.c_str());

    const float left = origin.x + caption_w;
    const float right = value_x - 6.0f;
    const float width = right - left;

    if (width > 1.0f && count_ > 0) {
        double lo = lo_;
        double hi = hi_;
        if (std::isnan(lo) || std::isnan(hi)) {
            double mn = std::numeric_limits<double>::infinity();
            double mx = -std::numeric_limits<double>::infinity();
            for (int i = 0; i < count_; ++i) {
                mn = std::min(mn, ring_[i]);
                mx = std::max(mx, ring_[i]);
            }
            if (std::isnan(lo)) lo = mn;
            if (std::isnan(hi)) hi = mx;
        }
        if (hi - lo < 1e-9) hi = lo + 1.0;

        const float base_y = origin.y + row_h - 2.0f;
        const float bar_h = row_h - 3.0f;

        dl->AddLine(ImVec2(left, base_y), ImVec2(right, base_y),
                    (dim & 0x00FFFFFFu) | (0x50u << 24), 1.0f);

        const int start = (count_ < kCapacity) ? 0 : write_;
        const ImU32 bar = (color & 0x00FFFFFFu) | (0xC8u << 24);
        const int columns = static_cast<int>(width);

        for (int x = 0; x < columns; ++x) {
            const int i = static_cast<int>(
                static_cast<long long>(x) * count_ / columns);
            const double v = ring_[(start + i) % kCapacity];
            const double t = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
            const float h = std::max(1.0f, static_cast<float>(t * bar_h));
            const float px = left + static_cast<float>(x);
            dl->AddLine(ImVec2(px, base_y), ImVec2(px, base_y - h), bar, 1.0f);
        }
    }

    ImGui::Dummy(ImVec2(avail, row_h));
}

} // namespace std42::ui
