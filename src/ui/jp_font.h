#pragma once
// Japanese glyph support for the decoder panel.
//
// SDR++ builds its font atlas from Roboto with only the default and Cyrillic
// glyph ranges (core/src/gui/style.cpp), so decoded Japanese text renders as
// blank boxes in the stock UI. This adds a second ImFont covering the Japanese
// ranges and re-uploads the atlas texture.
//
// The atlas may only be rebuilt outside a frame, so init() must be called from
// ModuleManager::Instance::postInit(), which SDR++ runs at the end of
// MainWindow::init() — before the render loop starts. A font path chosen in
// the UI therefore takes effect at the next start, not immediately.
//
// The two backend entry points needed to re-upload the texture are resolved
// with dlsym() rather than linked, so a build against a backend that does not
// export them still loads: the plugin simply reports that Japanese rendering
// is unavailable.

#include <string>
#include <vector>

struct ImFont;

namespace std42::ui::jp_font {

// Tries `preferred_path` first (may be empty), then the platform defaults.
// Safe to call more than once; only the first call does any work.
void init(const std::string& preferred_path);

// Non-null once init() has succeeded.
ImFont* font();

inline bool available() { return font() != nullptr; }

// Path actually loaded, or empty.
const std::string& loaded_path();

// Human-readable reason init() did not produce a font.
const std::string& error();

// Candidate paths probed when no explicit path is configured.
std::vector<std::string> default_candidates();

// RAII helper: pushes the Japanese font when one is available.
class Scoped {
public:
    Scoped();
    ~Scoped();
    Scoped(const Scoped&) = delete;
    Scoped& operator=(const Scoped&) = delete;

private:
    bool pushed_ = false;
};

} // namespace std42::ui::jp_font
