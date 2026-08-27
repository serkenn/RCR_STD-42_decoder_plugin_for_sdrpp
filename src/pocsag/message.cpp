#include "pocsag/message.h"

#include "pocsag/sjis.h"

#include <algorithm>

namespace std42::pocsag {

// 0-9, 予備, U (緊急表示), 空白, ハイフン, ']', '['  — 図3.6-1.
const char kNumericChars[17] = "0123456789*U -][";

double numeric_special_fraction(const std::string& text) {
    // Trailing 予備 and space codes are how a message is padded out to the
    // codeword boundary, so they say nothing about the content. Counting them
    // put the time broadcast — "…01034****", five specials in thirty
    // characters — over the threshold and had it reported as binary.
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '*' || text[end - 1] == ' ')) --end;

    int total = 0, special = 0;
    for (size_t i = 0; i < end; ++i) {
        const char c = text[i];
        if (c == ' ') continue;                 // interior padding
        ++total;
        if (c == '*' || c == 'U' || c == '-' || c == ']' || c == '[') ++special;
    }
    return total > 0 ? static_cast<double>(special) / total : 0.0;
}

const char* to_string(Format f) {
    switch (f) {
        case Format::Binary:       return "Binary";
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
        || (cp >= 0xFF01 && cp <= 0xFF5E)     // full-width forms, incl. ０-９
        || cp == 0x3000                       // ideographic space
        || cp == 0x3001 || cp == 0x3002;      // 、 。
}

// Decodes forward from `start` until the stream stops looking like text.
//
// CR and LF are line breaks *inside* the message, not terminators: an
// announcement arrives as several CRLF-separated lines, and treating the first
// one as the end of the text threw away everything after the opening sentence.
// NUL ends the text — that is what pads the payload out to the codeword
// boundary.
void decode_run(const std::vector<uint8_t>& b, size_t start,
                std::string& out, int& letters, int& other) {
    out.clear();
    letters = 0;
    other = 0;
    size_t i = start;
    while (i < b.size()) {
        const uint8_t c = b[i];
        if (c == 0x00) break;
        if (c == 0x0A || c == 0x0D) {
            // Fold CRLF into one newline; a bare CR or LF becomes one too.
            if (c == 0x0D && i + 1 < b.size() && b[i + 1] == 0x0A) ++i;
            out.push_back('\n');
            ++i;
            continue;
        }
        if (c == 0x09) { out.push_back(' '); ++other; ++i; continue; }
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
            ++other;
            ++i;
        } else if (c >= 0xA1 && c <= 0xDF) {
            const uint32_t cp = 0xFF61u + (c - 0xA1u);
            append_utf8(out, cp);
            if (is_japanese_letter(cp)) ++letters; else ++other;
            ++i;
        } else if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) &&
                   i + 1 < b.size()) {
            const uint16_t ucs =
                sjis_lookup(static_cast<uint16_t>((c << 8) | b[i + 1]));
            if (ucs == 0) break;
            append_utf8(out, ucs);
            if (is_japanese_letter(ucs)) ++letters; else ++other;
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

    double best_score = 0.0;
    auto consider = [&](const std::vector<uint8_t>& data, KanjiByteOrder o) {
        std::string text;
        int letters = 0, other = 0;
        // Every offset, not every other one. Shift-JIS is two-byte aligned
        // relative to the start of the text, but the text does not start at an
        // even offset within the payload — observed headers are 57 and 125
        // bytes long. Scanning only even offsets landed one byte late and ate
        // the first character of the announcement.
        for (size_t off = 0; off < limit; ++off) {
            decode_run(data, off, text, letters, other);
            if (text.empty() || letters <= 0) continue;
            const int chars = utf8_length(text);
            if (chars <= 0) continue;
            const double density =
                static_cast<double>(letters) / (letters + other);
            // Weighting length by density is what keeps the start from
            // creeping backwards into the header: reading a few more bytes of
            // binary as obscure kanji raises the letter count, but it costs
            // more in density than it gains.
            const double score = letters * density;
            if (score <= best_score) continue;
            best_score = score;
            best.text = text;
            best.letters = letters;
            best.density = density;
            best.offset = static_cast<int>(off);
            best.order = o;
        }
    };

    // Trailing separators and padding are framing, not content.
    auto trim = [](std::string& t) {
        while (!t.empty()) {
            const char c = t.back();
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') t.pop_back();
            else break;
        }
        size_t lead = 0;
        while (lead < t.size() && (t[lead] == ' ' || t[lead] == '\n')) ++lead;
        if (lead) t.erase(0, lead);
    };

    if (order != KanjiByteOrder::Swapped) consider(bytes, KanjiByteOrder::Normal);
    if (order != KanjiByteOrder::Normal) {
        consider(swap_pairs(bytes), KanjiByteOrder::Swapped);
    }
    trim(best.text);
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
    // A clean reading of one or two characters out of a large payload is the
    // ETX terminator landing early in binary data, not a short page.
    if (alpha.chars >= kMinFallbackChars && alpha.invalid == 0) return alpha;

    DecodedText numeric = decode_numeric(bits);
    // Fall back to whichever reading has fewer unmappable characters.
    if (alpha.chars > 0 && score(alpha) < score(kanji)) return alpha;
    if (kanji.chars >= kMinFallbackChars && kanji.invalid == 0) return kanji;

    // Last resort. The numeric table maps every code, so reaching here does
    // not mean the payload is numeric — only that nothing else fitted. Say so
    // rather than handing back a digit string that reads like a message.
    if (numeric.chars >= kNumericMinCharsToJudge &&
        numeric_special_fraction(numeric.text) > kNumericMaxSpecialFraction) {
        DecodedText d;
        d.format = Format::Binary;
        d.chars = numeric.chars;
        return d;
    }
    return numeric;
}

} // namespace std42::pocsag
