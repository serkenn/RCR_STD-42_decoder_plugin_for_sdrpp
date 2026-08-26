#include "ui/jp_font.h"

#include <imgui.h>
#include <gui/style.h>

#include <dlfcn.h>

#include <filesystem>

namespace std42::ui::jp_font {

namespace {

ImFont* g_font = nullptr;
std::string g_path;
std::string g_error;
bool g_done = false;

using CreateFontsTextureFn = bool (*)();
using DestroyFontsTextureFn = void (*)();

// First name that resolves in the already-loaded images wins.
void* resolve(const char* mangled, const char* plain) {
    if (void* p = ::dlsym(RTLD_DEFAULT, mangled)) return p;
    return ::dlsym(RTLD_DEFAULT, plain);
}

} // namespace

std::vector<std::string> default_candidates() {
    return {
#if defined(__APPLE__)
        "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc",
        "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
#else
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKjp-Regular.otf",
        "/usr/share/fonts/opentype/noto/NotoSansJP-Regular.otf",
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
        "/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf",
        "/usr/share/fonts/truetype/takao-gothic/TakaoPGothic.ttf",
        "/usr/share/fonts/truetype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/google-noto/NotoSansCJKjp-Regular.otf",
#endif
    };
}

void init(const std::string& preferred_path) {
    if (g_done) return;
    g_done = true;

    // Resolve the backend entry points before touching the atlas: if they are
    // missing there is no way to re-upload the texture, and adding a font
    // would corrupt the glyphs SDR++ already draws.
    //
    // imgui_impl_opengl3.cpp is compiled as C++ and its header does not wrap
    // the declarations in extern "C", so sdrpp_core exports these under their
    // Itanium-mangled names ("_Z36…v"). The mangling is identical on Mach-O and
    // ELF, and dlsym() on macOS strips the leading underscore nm shows. The
    // plain C spelling is tried as well, for a backend built with extern "C".
    auto destroy_tex = reinterpret_cast<DestroyFontsTextureFn>(
        resolve("_Z37ImGui_ImplOpenGL3_DestroyFontsTexturev",
                "ImGui_ImplOpenGL3_DestroyFontsTexture"));
    auto create_tex = reinterpret_cast<CreateFontsTextureFn>(
        resolve("_Z36ImGui_ImplOpenGL3_CreateFontsTexturev",
                "ImGui_ImplOpenGL3_CreateFontsTexture"));
    if (!destroy_tex || !create_tex) {
        g_error = "ImGui OpenGL backend entry points not found; "
                  "Japanese text cannot be rendered in the panel.";
        return;
    }

    std::string chosen;
    std::error_code ec;
    if (!preferred_path.empty() && std::filesystem::exists(preferred_path, ec)) {
        chosen = preferred_path;
    } else {
        if (!preferred_path.empty()) {
            g_error = "configured font not found: " + preferred_path + "; ";
        }
        for (const std::string& c : default_candidates()) {
            if (std::filesystem::exists(c, ec)) { chosen = c; break; }
        }
    }
    if (chosen.empty()) {
        g_error += "no Japanese font found (set one in the panel; on Ubuntu, "
                   "apt install fonts-noto-cjk).";
        return;
    }

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;

    // Default range plus the Japanese set (kana, punctuation, full-width forms
    // and ~3000 common kanji). The full CJK block would be an order of
    // magnitude larger for glyphs municipal traffic effectively never uses.
    static ImVector<ImWchar> ranges;
    ranges.clear();
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(atlas->GetGlyphRangesDefault());
    builder.AddRanges(atlas->GetGlyphRangesJapanese());
    builder.BuildRanges(&ranges);

    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;

    ImFont* f = atlas->AddFontFromFileTTF(chosen.c_str(), 16.0f * style::uiScale,
                                          &cfg, ranges.Data);
    if (!f) {
        g_error += "failed to load font: " + chosen;
        return;
    }

    if (!atlas->Build()) {
        g_error += "font atlas build failed for: " + chosen;
        return;
    }

    // Re-upload: the existing GL texture still holds the pre-rebuild atlas.
    destroy_tex();
    if (!create_tex()) {
        g_error += "font atlas texture upload failed";
        return;
    }

    g_font = f;
    g_path = chosen;
    g_error.clear();
}

ImFont* font() { return g_font; }
const std::string& loaded_path() { return g_path; }
const std::string& error() { return g_error; }

Scoped::Scoped() {
    if (g_font) {
        ImGui::PushFont(g_font);
        pushed_ = true;
    }
}

Scoped::~Scoped() {
    if (pushed_) ImGui::PopFont();
}

} // namespace std42::ui::jp_font
