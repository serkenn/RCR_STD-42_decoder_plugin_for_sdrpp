#pragma once
// Message payload decoding, per RCR STD-42 §3.6.
//
// A message is the concatenation of the 20 information bits (codeword bits
// 2..21) of every message codeword that follows an address codeword. Those
// bits arrive MSB first, but *within* a character the spec is explicit that
// "各文字は、LSBから順次伝送される" — characters are sent LSB first — so every
// character field has to be bit-reversed before it means anything.
//
//   §3.6.1 数字      4 bits per character, mapped through 図3.6-1
//   §3.6.2 カナ英数字 8 bits per character, JIS X 0201
//   §3.6.3 漢字      16 bits per character, Shift-JIS (JIS C 6226)

#include <cstdint>
#include <string>
#include <vector>

namespace std42::pocsag {

// Binary is not one of STD-42's message formats. It is what this decoder
// reports when a payload reads as none of them: the numeric format maps all 16
// of its codes, so it never fails outright and will render any payload as a
// digit string, which looks like a decoded message and is not one.
enum class Format { Auto, Numeric, Alphanumeric, Kanji, Binary };

// §3.6.3 says the kanji format transmits a *16-bit* code LSB first, which puts
// the Shift-JIS trailing byte on the wire before the leading byte. Deployed
// equipment is not consistent about this — much of it simply sends each byte
// LSB first in reading order — so the byte order is selectable and defaults to
// picking whichever decodes cleanly.
enum class KanjiByteOrder { Auto, Normal, Swapped };

const char* to_string(Format f);
const char* to_string(KanjiByteOrder o);

struct DecodedText {
    Format format = Format::Numeric;
    KanjiByteOrder byte_order = KanjiByteOrder::Normal;
    std::string text;         // UTF-8
    int chars = 0;
    int double_byte = 0;
    int invalid = 0;          // unmappable characters
    // Bytes of binary prefix skipped to reach the text, when the payload was
    // not text from its first byte. 0 for an ordinary message.
    int header_bytes = 0;
};

// A run of real Japanese found inside a payload.
//
// Municipal traffic does not always put text at byte 0. Observed broadcasts
// from a Karatsu address carry a 28-byte binary header, a short title, and
// then the announcement, so decoding the payload from the start yields the
// header as mojibake and only stumbles into the text once the byte alignment
// happens to come right. Locating the text directly recovers it intact.
struct JapaneseRun {
    std::string text;
    int letters = 0;                 // kana + kanji, the marker of real prose
    double density = 0.0;            // letters per character
    int offset = 0;                  // byte offset the run started at
    KanjiByteOrder order = KanjiByteOrder::Normal;
};

// Best run over both byte orders and every 2-byte-aligned offset. `order`
// restricts the search when it is not Auto.
JapaneseRun find_japanese_run(const std::vector<uint8_t>& bytes,
                              KanjiByteOrder order = KanjiByteOrder::Auto);

// A run must clear both to be believed. Measured against 76 off-air messages,
// the two that genuinely carried an announcement scored 37 letters at density
// 1.00, while the best coincidental run in the other 74 reached 9 letters at
// 0.71 — so these sit in a wide gap rather than on a boundary.
inline constexpr int kJapaneseRunMinLetters = 12;
inline constexpr double kJapaneseRunMinDensity = 0.5;

// Ten of the sixteen numeric codes are digits; the rest are 予備 (reserved), U,
// space, hyphen and the two brackets (図3.6-1). Genuine numeric traffic uses
// those sparingly as delimiters, while a binary payload pushed through the
// table hits them at close to their share of the alphabet. Measured over 80
// off-air messages the two populations do not overlap: real ones ran 0.038 to
// 0.109, payloads with no readable content 0.133 to 0.233.
inline constexpr double kNumericMaxSpecialFraction = 0.12;
// Below this many characters the fraction is too noisy to judge; the shortest
// genuine numeric message observed is 30.
inline constexpr int kNumericMinCharsToJudge = 24;

// A fallback reading shorter than this is noise, not a short message.
inline constexpr int kMinFallbackChars = 4;

// Fraction of `text` made up of the non-digit numeric codes.
double numeric_special_fraction(const std::string& text);

// §3.6.1 図3.6-1: 4-bit code → displayed character. Index 10 is 予備
// (reserved) and is rendered as '*'.
extern const char kNumericChars[17];

// Bit-reversal of the low `width` bits of `v`.
uint32_t reverse_bits(uint32_t v, int width);

// `bits` holds one message bit per element, in transmission order.
DecodedText decode_numeric(const std::vector<uint8_t>& bits);
DecodedText decode_alphanumeric(const std::vector<uint8_t>& bits);
DecodedText decode_kanji(const std::vector<uint8_t>& bits, KanjiByteOrder order);

// Applies `want`, or picks the most plausible format when `want` is Auto.
DecodedText decode_message(const std::vector<uint8_t>& bits,
                           Format want,
                           KanjiByteOrder order);

// Packs `bits` into bytes, each character field LSB first (§3.6.2/§3.6.3).
// Trailing bits that do not fill a byte are discarded.
std::vector<uint8_t> bits_to_bytes(const std::vector<uint8_t>& bits);

} // namespace std42::pocsag
