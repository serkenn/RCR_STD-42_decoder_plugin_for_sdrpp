#include "pocsag/sjis.h"

#include <algorithm>

namespace std42::pocsag {

uint16_t sjis_lookup(uint16_t code) {
    const SjisEntry* first = kSjisTable;
    const SjisEntry* last = kSjisTable + kSjisTableSize;
    const SjisEntry* it = std::lower_bound(
        first, last, code,
        [](const SjisEntry& e, uint16_t v) { return e.sjis < v; });
    if (it != last && it->sjis == code) return it->ucs;
    return 0;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool jisx0201_to_utf8(uint8_t b, std::string& out) {
    if (b == 0x0A || b == 0x0D) { out.push_back('\n'); return true; }
    if (b >= 0x20 && b <= 0x7E) {
        // JIS X 0201 differs from ASCII at two positions. Municipal traffic
        // treats them as ASCII in practice, so they are passed through.
        out.push_back(static_cast<char>(b));
        return true;
    }
    if (b >= 0xA1 && b <= 0xDF) {
        // Halfwidth katakana block, contiguous at U+FF61.
        append_utf8(out, 0xFF61u + (b - 0xA1u));
        return true;
    }
    return false;
}

SjisDecodeResult sjis_decode(const std::vector<uint8_t>& bytes) {
    SjisDecodeResult r;
    r.text.reserve(bytes.size() * 2);

    for (size_t i = 0; i < bytes.size();) {
        const uint8_t b = bytes[i];

        // Single-byte ranges: ASCII / JIS X 0201 roman and halfwidth katakana.
        if (b < 0x80 || (b >= 0xA1 && b <= 0xDF)) {
            if (b == 0x00) { ++i; continue; }               // padding
            if (jisx0201_to_utf8(b, r.text)) {
                ++r.chars;
            } else {
                append_utf8(r.text, 0xFFFDu);
                ++r.chars;
                ++r.invalid;
            }
            ++i;
            continue;
        }

        // Double-byte lead: 0x81..0x9F or 0xE0..0xFC.
        const bool lead = (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
        if (lead && i + 1 < bytes.size()) {
            const uint8_t t = bytes[i + 1];
            const bool trail = (t >= 0x40 && t <= 0xFC && t != 0x7F);
            if (trail) {
                const uint16_t ucs =
                    sjis_lookup(static_cast<uint16_t>((b << 8) | t));
                if (ucs != 0) {
                    append_utf8(r.text, ucs);
                    ++r.chars;
                    ++r.double_byte;
                } else {
                    append_utf8(r.text, 0xFFFDu);
                    ++r.chars;
                    ++r.invalid;
                }
                i += 2;
                continue;
            }
        }

        append_utf8(r.text, 0xFFFDu);
        ++r.chars;
        ++r.invalid;
        ++i;
    }
    return r;
}

} // namespace std42::pocsag
