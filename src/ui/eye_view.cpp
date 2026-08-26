#include "ui/eye_view.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace std42::ui {

EyeView::EyeView(EyePull pull) : pull_(std::move(pull)) {
    scratch_.resize(kPullPerFrame);
}

void EyeView::reset() {
    std::fill(std::begin(accum_), std::end(accum_), 0.0f);
    peak_ = 1.0f;
    opening_ = 0.0;
}

void EyeView::accumulate(int n) {
    // Statistics for the eye-opening figure, gathered at the sampling instant.
    double pos_sum = 0.0, pos_sq = 0.0, neg_sum = 0.0, neg_sq = 0.0;
    int pos_n = 0, neg_n = 0;

    for (int i = 0; i < n; ++i) {
        const demod::EyePoint& p = scratch_[static_cast<size_t>(i)];

        const float t = std::clamp((p.value + kValueMax) / (2.0f * kValueMax),
                                   0.0f, 0.999f);
        const int by = static_cast<int>((1.0f - t) * kBinsY);

        // Fold over two symbol periods so the eye closes on both sides.
        for (int rep = 0; rep < 2; ++rep) {
            const float x = (p.phase + static_cast<float>(rep)) * 0.5f;
            const int bx = static_cast<int>(std::clamp(x, 0.0f, 0.999f) * kBinsX);
            float& cell = accum_[by * kBinsX + bx];
            cell += 1.0f;
            peak_ = std::max(peak_, cell);
        }

        if (std::fabs(p.phase - 0.5f) < 0.06f) {
            if (p.value >= 0.0f) { pos_sum += p.value; pos_sq += double(p.value) * p.value; ++pos_n; }
            else                 { neg_sum += p.value; neg_sq += double(p.value) * p.value; ++neg_n; }
        }
    }

    if (pos_n > 4 && neg_n > 4) {
        const double pm = pos_sum / pos_n;
        const double nm = neg_sum / neg_n;
        const double ps = std::sqrt(std::max(0.0, pos_sq / pos_n - pm * pm));
        const double ns = std::sqrt(std::max(0.0, neg_sq / neg_n - nm * nm));
        const double spread = ps + ns;
        const double v = (spread > 1e-6) ? (pm - nm) / spread : 0.0;
        opening_ += (v - opening_) * 0.15;      // smooth the readout
    }
}

void EyeView::draw(float width, float height) {
    for (auto& c : accum_) c *= kDecay;
    peak_ *= kDecay;
    if (peak_ < 1.0f) peak_ = 1.0f;

    if (pull_) {
        const int n = pull_(scratch_.data(), kPullPerFrame);
        accumulate(n);
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                      IM_COL32(12, 14, 18, 255), 3.0f);

    const float cw = width / kBinsX;
    const float ch = height / kBinsY;
    const float inv_peak = 1.0f / peak_;

    for (int y = 0; y < kBinsY; ++y) {
        for (int x = 0; x < kBinsX; ++x) {
            const float v = accum_[y * kBinsX + x];
            if (v <= 0.01f) continue;
            // Compress the dynamic range so faint traces stay visible.
            const float t = std::clamp(std::sqrt(v * inv_peak), 0.0f, 1.0f);
            const ImU32 col = IM_COL32(
                static_cast<int>(40 + 40 * t),
                static_cast<int>(90 + 165 * t),
                static_cast<int>(70 + 60 * t),
                static_cast<int>(30 + 225 * t));
            dl->AddRectFilled(
                ImVec2(origin.x + x * cw, origin.y + y * ch),
                ImVec2(origin.x + (x + 1) * cw + 0.6f, origin.y + (y + 1) * ch + 0.6f),
                col);
        }
    }

    // Decision threshold.
    const float mid_y = origin.y + height * 0.5f;
    dl->AddLine(ImVec2(origin.x, mid_y), ImVec2(origin.x + width, mid_y),
                IM_COL32(120, 130, 145, 110), 1.0f);

    // Sampling instants: phase 0.5 within each of the two folded periods.
    for (float fx : {0.25f, 0.75f}) {
        const float px = origin.x + width * fx;
        for (float y = origin.y + 2.0f; y < origin.y + height - 2.0f; y += 6.0f) {
            dl->AddLine(ImVec2(px, y), ImVec2(px, std::min(y + 3.0f, origin.y + height - 2.0f)),
                        IM_COL32(200, 200, 210, 90), 1.0f);
        }
    }

    dl->AddRect(origin, ImVec2(origin.x + width, origin.y + height),
                IM_COL32(70, 76, 88, 255), 3.0f);

    ImGui::Dummy(ImVec2(width, height));
}

} // namespace std42::ui
