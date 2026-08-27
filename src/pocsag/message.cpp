#include "pocsag/message.h"

#include "pocsag/sjis.h"

#include <algorithm>

namespace std42::pocsag {

// 0-9, 予備, U (緊急表示), 空白, ハイフン, ']', '['  — 図3.6-1.
const char kNumericChars[17] = "0123456789*U -][";

const char* to_string(Format f) {
    switch (f) {
        case Format::Numeric:      return "Numeric";
        case Format::Alphanumeric: return "Alphanumeric";
        case Format::Kanji:        return "Kanji";
        case Format::Auto:
        default:                   return "Auto";
    }
}

const char* to_string(KanjiByteOrder o) {
    switch (o) {
        case KanjiByteOrder::Normal:  return "Normal";
        case KanjiByteOrder::Swapped: return "Swapped";
        case KanjiByteOrder::Auto:
        default:                      return "Auto";
    }
}

uint32_t reverse_bits(uint32_t v, int width) {
    uint32_t r = 0;
    for (int i = 0; i < width; ++i) {
        r = (r << 1) | ((v >> i) & 1u);
    }
    return r;
}

std::vector<uint8_t> bits_to_bytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out;
    out.reserve(bits.size() / 8);
    const size_t n = bits.size() & ~size_t(7);
    for (size_t i = 0; i < n; i += 8) {
        uint8_t b = 0;
        // The character's LSB is transmitted first.
        for (int k = 0; k < 8; ++k) {
            if (bits[i + static_cast<size_t>(k)]) b |= static_cast<uint8_t>(1u << k);
        }
        out.push_back(b);
    }
    return out;
}

namespace {

std::vector<uint8_t> swap_pairs(std::vector<uint8_t> v) {
    for (size_t i = 0; i + 1 < v.size(); i += 2) {
        const uint8_t t = v[i];
        v[i] = v[i + 1];
        v[i + 1] = t;
    }
    return v;
}

// Messages are padded out to the codeword boundary. Numeric pads with spaces
// (§3.6.1); text formats are padded with NUL or spaces and frequently carry an
// explicit ETX terminator, so the tail is trimmed before display.
void trim_tail(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        if (c == ' ' || c == '\0' || c == '\n' || c == '\r' || c == '\t') s.pop_back();
        else break;
    }
}

// Truncates the byte stream at an ETX / EOT terminator, if present.
void cut_at_terminator(std::vector<uint8_t>& bytes) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == 0x03 || bytes[i] == 0x04) {
            bytes.resize(i);
            return;
        }
    }
}

// Lower is better: unmappable characters dominate, then a mild preference for
// results that actually produced double-byte characters.
double score(const DecodedText& d) {
    if (d.chars == 0) return 1e9;
    return static_cast<double>(d.invalid) / d.chars
           - 0.001 * static_cast<double>(d.double_byte);
}

} // namespace

namespace {

// Kana, kanji and the punctuation that goes with them. Latin letters and
// digits deliberately do not count: they occur just as readily in mojibake, so
// only the marks that indicate genuine Japanese prose are scored.
bool is_japanese_letter(uint32_t cp) {
    return (cp >= 0x3040 && cp <= 0x30FF)     // hiragana + katakana
        || (cp >= 0x4E00 && cp <= 0x9FFF)     // CJK unified ideographs
        || (cp >= 0xFF61 && cp <= 0xFF9F)     // half-width katakana
        || cp == 0x3001 || cp == 0x3002       // 、 。
        || cp == 0xFF01 || cp == 0xFF1F;      // ！ ？
}

// Decodes forward from `start` until the stream stops looking like text.
// The observed framing separates fields with NUL and TAB, so those terminate a
// run rather than being decoded through.
void decode_run(const std::vector<uint8_t>& b, size_t start,
                std::string& out, int& letters) {
    out.clear();
    letters = 0;
    size_t i = start;
    while (i < b.size()) {
        const uint8_t c = b[i];
        if (c == 0x00 || c == 0x09 || c == 0x0A || c == 0x0D) break;
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            ++i;
        } else if (c >= 0xA1 && c <= 0xDF) {
            const uint32_t cp = 0xFF61u + (c - 0xA1u);
            append_utf8(out, cp);
            if (is_japanese_letter(cp)) ++letters;
            ++i;
        } else if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) &&
                   i + 1 < b.size()) {
            const uint16_t ucs =
                sjis_lookup(static_cast<uint16_t>((c << 8) | b[i + 1]));
            if (ucs == 0) break;
            append_utf8(out, ucs);
            if (is_japanese_letter(ucs)) ++letters;
            i += 2;
        } else {
            break;
        }
    }
}

// Counts characters, not bytes, so density compares like with like.
int utf8_length(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

} // namespace

JapaneseRun find_japanese_run(const std::vector<uint8_t>& bytes,
                              KanjiByteOrder order) {
    JapaneseRun best;
    if (bytes.size() < 4) return best;

    // Scanning the whole payload would be wasteful and pointless: a text body
    // that starts hundreds of bytes in is not what these messages look like.
    const size_t limit = std::min<size_t>(bytes.size(), 320);

    auto consider = [&](const std::vector<uint8_t>& data, KanjiByteOrder o) {
        std::string text;
        int letters = 0;
        // Shift-JIS is two-byte aligned from the start of the text, so odd
        // offsets cannot be the beginning of a character.
        for (size_t off = 0; off < limit; off += 2) {
            decode_run(data, off, text, letters);
            if (text.empty() || letters <= best.letters) continue;
            const int chars = utf8_length(text);
            if (chars <= 0) continue;
            best.text = text;
            best.letters = letters;
            best.density = static_cast<double>(letters) / chars;
            best.offset = static_cast<int>(off);
            best.order = o;
        }
    };

    if (order != KanjiByteOrder::Swapped) consider(bytes, KanjiByteOrder::Normal);
    if (order != KanjiByteOrder::Normal) {
        consider(swap_pairs(bytes), KanjiByteOrder::Swapped);
    }
    return best;
}

DecodedText decode_numeric(const std::vector<uint8_t>& bits) {
    DecodedText d;
    d.format = Format::Numeric;
    const size_t n = bits.size() & ~size_t(3);
    d.text.reserve(n / 4);
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            if (bits[i + static_cast<size_t>(k)]) v |= 1u << k;   // LSB first
        }
        d.text.push_back(kNumericChars[v & 0xFu]);
        ++d.chars;
    }
    trim_tail(d.text);
    return d;
}

DecodedText decode_alphanumeric(const std::vector<uint8_t>& bits) {
    DecodedText d;
    d.format = Format::Alphanumeric;

    std::vector<uint8_t> bytes = bits_to_bytes(bits);
    cut_at_terminator(bytes);

    d.text.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        if (b == 0x00) continue;                 // padding
        if (jisx0201_to_utf8(b, d.text)) {
            ++d.chars;
        } else {
            append_utf8(d.text, 0xFFFDu);
            ++d.chars;
            ++d.invalid;
        }
    }
    trim_tail(d.text);
    return d;
}

DecodedText decode_kanji(const std::vector<uint8_t>& bits, KanjiByteOrder order) {
    std::vector<uint8_t> bytes = bits_to_bytes(bits);
    cut_at_terminator(bytes);

    auto run = [&](KanjiByteOrder o) {
        DecodedText d;
        d.format = Format::Kanji;
        d.byte_order = o;
        const SjisDecodeResult r =
            sjis_decode(o == KanjiByteOrder::Swapped ? swap_pairs(bytes) : bytes);
        d.text = r.text;
        d.chars = r.chars;
        d.double_byte = r.double_byte;
        d.invalid = r.invalid;
        trim_tail(d.text);
        return d;
    };

    if (order != KanjiByteOrder::Auto) return run(order);

    DecodedText normal = run(KanjiByteOrder::Normal);
    DecodedText swapped = run(KanjiByteOrder::Swapped);
    return score(swapped) < score(normal) ? swapped : normal;
}

DecodedText decode_message(const std::vector<uint8_t>& bits,
                           Format want,
                           KanjiByteOrder order) {
    if (want == Format::Numeric) return decode_numeric(bits);
    if (want == Format::Alphanumeric) return decode_alphanumeric(bits);

    // Kanji, or Auto: look for a run of real Japanese anywhere in the payload
    // first. Decoding from byte 0 works only when the message is text all the
    // way through, and municipal announcements are not — they arrive behind a
    // binary header, which turns the whole reading to mojibake and leaves the
    // announcement itself half-visible at best.
    {
        const std::vector<uint8_t> bytes = bits_to_bytes(bits);
        const JapaneseRun run = find_japanese_run(bytes, order);
        if (run.letters >= kJapaneseRunMinLetters &&
            run.density >= kJapaneseRunMinDensity) {
            DecodedText d;
            d.format = Format::Kanji;
            d.byte_order = run.order;
            d.text = run.text;
            d.header_bytes = run.offset;
            for (unsigned char c : d.text) {
                if ((c & 0xC0) != 0x80) ++d.chars;
                if ((c & 0xF0) == 0xE0) ++d.double_byte;
            }
            return d;
        }
    }

    if (want == Format::Kanji) return decode_kanji(bits, order);

    // Auto. Shift-JIS is an ASCII superset, so a message that decodes cleanly
    // under both only counts as kanji when it actually contains a double-byte
    // character; otherwise the simpler JIS X 0201 reading is reported.
    const DecodedText kanji = decode_kanji(bits, order);
    if (kanji.chars > 0 && kanji.double_byte > 0 &&
        kanji.invalid * 10 <= kanji.chars) {
        return kanji;
    }

    const DecodedText alpha = decode_alphanumeric(bits);
    if (alpha.chars > 0 && alpha.invalid == 0) return alpha;

    const DecodedText numeric = decode_numeric(bits);
    // Fall back to whichever reading has fewer unmappable characters.
    if (alpha.chars > 0 && score(alpha) < score(kanji)) return alpha;
    if (kanji.chars > 0 && kanji.invalid * 4 <= kanji.chars) return kanji;
    return numeric;
}

} // namespace std42::pocsag
