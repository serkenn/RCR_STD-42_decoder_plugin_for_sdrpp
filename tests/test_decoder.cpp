// End-to-end self-test: synthesise an RCR STD-42 transmission, modulate it to
// complex baseband, and check that the receiver recovers the original text.
//
// Build and run with tests/run.sh.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "pocsag/bch.h"
#include "pocsag/framer.h"
#include "pocsag/interpret.h"
#include "pocsag/message.h"
#include "pocsag/receiver.h"
#include "pocsag/sjis.h"

using namespace std42;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) ++g_failures;
}

// ── Transmitter ───────────────────────────────────────────────────────────

int parity32(uint32_t v) { return __builtin_parity(v); }

// Inverse of pocsag::bch_correct: build a valid codeword from 21 info bits.
uint32_t encode_codeword(uint32_t info21) {
    uint32_t cw = (info21 & 0x1FFFFFu) << 11;
    uint32_t s = cw >> 1;
    for (int i = 30; i >= 10; --i) {
        if (s & (1u << i)) s ^= pocsag::kBchGenerator << (i - 10);
    }
    cw |= (s & 0x3FFu) << 1;
    if (parity32(cw)) cw |= 1u;
    return cw;
}

uint32_t address_codeword(uint32_t address21, int function) {
    // Flag 0, bits 2..19 the top 18 address bits, bits 20..21 the function.
    const uint32_t addr18 = (address21 >> 3) & 0x3FFFFu;
    return encode_codeword((addr18 << 2) | (static_cast<uint32_t>(function) & 3u));
}

uint32_t message_codeword(uint32_t payload20) {
    return encode_codeword((1u << 20) | (payload20 & 0xFFFFFu));
}

void push_word(std::vector<uint8_t>& bits, uint32_t w) {
    for (int i = 31; i >= 0; --i) bits.push_back((w >> i) & 1u);
}

// UTF-8 → code points.
std::vector<uint32_t> utf8_to_cps(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0)      { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        for (int k = 0; k < extra && i + 1 + k < s.size(); ++k) {
            cp = (cp << 6) | (s[i + 1 + k] & 0x3F);
        }
        out.push_back(cp);
        i += 1 + extra;
    }
    return out;
}

// Reverse lookup through the shipped table — also proves the table round-trips.
uint16_t ucs_to_sjis(uint32_t cp) {
    for (unsigned i = 0; i < pocsag::kSjisTableSize; ++i) {
        if (pocsag::kSjisTable[i].ucs == cp) return pocsag::kSjisTable[i].sjis;
    }
    return 0;
}

std::vector<uint8_t> utf8_to_sjis_bytes(const std::string& s) {
    std::vector<uint8_t> out;
    for (uint32_t cp : utf8_to_cps(s)) {
        if (cp < 0x80) { out.push_back(static_cast<uint8_t>(cp)); continue; }
        const uint16_t sj = ucs_to_sjis(cp);
        if (sj == 0) { out.push_back('?'); continue; }
        out.push_back(static_cast<uint8_t>(sj >> 8));
        out.push_back(static_cast<uint8_t>(sj & 0xFF));
    }
    return out;
}

// Each character field goes out LSB first (§3.6.2 / §3.6.3).
std::vector<uint8_t> bytes_to_bits_lsb_first(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes) {
        for (int k = 0; k < 8; ++k) bits.push_back((b >> k) & 1u);
    }
    return bits;
}

std::vector<uint8_t> numeric_to_bits(const std::string& digits) {
    std::vector<uint8_t> bits;
    for (char c : digits) {
        uint32_t v = 12;                                   // space
        for (int i = 0; i < 16; ++i) {
            if (pocsag::kNumericChars[i] == c) { v = static_cast<uint32_t>(i); break; }
        }
        for (int k = 0; k < 4; ++k) bits.push_back((v >> k) & 1u);
    }
    return bits;
}

// Builds preamble + batches carrying one call.
std::vector<uint8_t> build_transmission(uint32_t address, int function,
                                        const std::vector<uint8_t>& payload_bits) {
    std::vector<uint8_t> bits;

    // §3.2: at least 576 bits of 1010…
    for (int i = 0; i < 600; ++i) bits.push_back((i % 2) ? 0 : 1);

    // Slot plan: the address codeword must land in the frame matching the low
    // three address bits (§3.3); message codewords follow it directly.
    std::vector<uint32_t> slots;
    const int frame = static_cast<int>(address & 7u);
    for (int i = 0; i < 2 * frame; ++i) slots.push_back(pocsag::kIdleCodeword);
    slots.push_back(address_codeword(address, function));

    for (size_t i = 0; i < payload_bits.size(); i += 20) {
        uint32_t w = 0;
        for (size_t k = 0; k < 20; ++k) {
            const uint8_t b = (i + k < payload_bits.size()) ? payload_bits[i + k] : 0;
            w = (w << 1) | b;                              // MSB first on air
        }
        slots.push_back(message_codeword(w));
    }
    // Pad the final batch out with idle codewords.
    while (slots.size() % pocsag::kCodewordsPerBatch != 0) {
        slots.push_back(pocsag::kIdleCodeword);
    }

    for (size_t i = 0; i < slots.size(); ++i) {
        if (i % pocsag::kCodewordsPerBatch == 0) push_word(bits, pocsag::kSyncCodeword);
        push_word(bits, slots[i]);
    }
    // A trailing sync so the framer sees the last batch close cleanly.
    push_word(bits, pocsag::kSyncCodeword);
    for (int i = 0; i < pocsag::kCodewordsPerBatch; ++i) {
        push_word(bits, pocsag::kIdleCodeword);
    }
    push_word(bits, pocsag::kSyncCodeword);
    return bits;
}

// 2-FSK at ±4.5 kHz (§2.1.6) on a complex baseband carrier.
std::vector<demod::Complex32> modulate(const std::vector<uint8_t>& bits,
                                       double sample_rate, double baud,
                                       double carrier_offset_hz,
                                       double noise_sigma,
                                       unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, noise_sigma);

    const double sps = sample_rate / baud;
    std::vector<demod::Complex32> out;
    out.reserve(static_cast<size_t>(bits.size() * sps) + 4096);

    double phase = 0.0;
    auto emit = [&](double freq_hz, int n) {
        for (int i = 0; i < n; ++i) {
            phase += 2.0 * M_PI * (freq_hz + carrier_offset_hz) / sample_rate;
            if (phase > M_PI) phase -= 2.0 * M_PI;
            out.emplace_back(
                static_cast<float>(std::cos(phase) + noise(rng)),
                static_cast<float>(std::sin(phase) + noise(rng)));
        }
    };

    emit(0.0, static_cast<int>(sps * 20));                 // quiet lead-in
    double acc = 0.0;
    for (uint8_t b : bits) {
        acc += sps;
        const int n = static_cast<int>(acc);
        acc -= n;
        // Mark (binary 1) is centre -4.5 kHz, space (binary 0) is +4.5 kHz.
        emit(b ? -4500.0 : 4500.0, n);
    }
    emit(0.0, static_cast<int>(sps * 20));
    return out;
}

// ── Harness ───────────────────────────────────────────────────────────────

struct RunResult {
    std::vector<pocsag::DecodedCall> calls;
};

RunResult run_receiver(const std::vector<demod::Complex32>& iq,
                       pocsag::BaudMode baud_mode,
                       pocsag::Format format,
                       pocsag::KanjiByteOrder order,
                       double sample_rate) {
    RunResult r;
    pocsag::Receiver rx(sample_rate,
                        [&](const pocsag::DecodedCall& c) { r.calls.push_back(c); });
    rx.set_baud_mode(baud_mode);
    rx.set_format(format);
    rx.set_kanji_byte_order(order);

    constexpr int kBlock = 1024;
    for (size_t i = 0; i < iq.size(); i += kBlock) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, iq.size() - i));
        rx.process(iq.data() + i, n);
    }
    return r;
}

// ── Tests ─────────────────────────────────────────────────────────────────

void test_bch() {
    pocsag::bch_init();

    const uint32_t cw = encode_codeword(0x0ABCDE);
    check(pocsag::bch_syndrome(cw) == 0, "BCH: encoded codeword has zero syndrome");
    check(parity32(cw) == 0, "BCH: encoded codeword has even parity");

    uint32_t v = cw;
    check(pocsag::bch_correct(v) == 0 && v == cw, "BCH: clean codeword unchanged");

    bool all_single = true, all_double = true;
    for (int i = 0; i < 32; ++i) {
        uint32_t e = cw ^ (1u << i);
        all_single &= (pocsag::bch_correct(e) == 1 && e == cw);
    }
    check(all_single, "BCH: every single-bit error corrected");

    for (int i = 0; i < 32; ++i) {
        for (int j = i + 1; j < 32; ++j) {
            uint32_t e = cw ^ (1u << i) ^ (1u << j);
            all_double &= (pocsag::bch_correct(e) == 2 && e == cw);
        }
    }
    check(all_double, "BCH: every double-bit error corrected");
}

void test_sync_words() {
    // 図3.4-2 and 図3.4-5, transcribed bit by bit from the standard.
    const char* sync = "01111100110100100001010111011000";
    const char* idle = "01111010100010011100000110010111";
    uint32_t s = 0, id = 0;
    for (int i = 0; i < 32; ++i) {
        s = (s << 1) | static_cast<uint32_t>(sync[i] - '0');
        id = (id << 1) | static_cast<uint32_t>(idle[i] - '0');
    }
    check(s == pocsag::kSyncCodeword, "Sync codeword matches 図3.4-2 (0x7CD215D8)");
    check(id == pocsag::kIdleCodeword, "Idle codeword matches 図3.4-5 (0x7A89C197)");
    check(pocsag::bch_syndrome(pocsag::kIdleCodeword) == 0 &&
          parity32(pocsag::kIdleCodeword) == 0,
          "Idle codeword is a valid BCH codeword");
}

void test_numeric_example() {
    // 図3.4-4: the numeric message "3681" occupies one message codeword.
    std::vector<uint8_t> bits = numeric_to_bits("3681");
    while (bits.size() < 20) {
        for (int k = 0; k < 4; ++k) bits.push_back((12u >> k) & 1u);   // space pad
    }
    const char* expect = "11000110000110000011";
    std::string got;
    for (int i = 0; i < 20; ++i) got.push_back(bits[i] ? '1' : '0');
    check(got == expect,
          "Numeric bit order matches 図3.4-4 for \"3681\" (got " + got + ")");

    const pocsag::DecodedText d = pocsag::decode_numeric(bits);
    check(d.text == "3681", "Numeric round-trip: \"3681\" -> \"" + d.text + "\"");
}

void test_end_to_end(const char* label, double baud, pocsag::BaudMode mode,
                     double offset_hz, double noise, unsigned seed) {
    const std::string text =
        "こちらは防災行政無線です。ただいま試験放送を行っています。";
    const uint32_t address = 1234567;
    const int function = 3;

    const std::vector<uint8_t> sjis_bits =
        bytes_to_bits_lsb_first(utf8_to_sjis_bytes(text));
    const std::vector<uint8_t> bits =
        build_transmission(address, function, sjis_bits);
    const auto iq = modulate(bits, 48000.0, baud, offset_hz, noise, seed);

    const RunResult r = run_receiver(iq, mode, pocsag::Format::Auto,
                                     pocsag::KanjiByteOrder::Auto, 48000.0);

    const std::string what = std::string(label);
    if (r.calls.empty()) {
        check(false, what + ": decoded a call");
        return;
    }
    const pocsag::DecodedCall& c = r.calls.front();
    check(c.address == address,
          what + ": address " + std::to_string(c.address) +
              " == " + std::to_string(address));
    check(c.function == function, what + ": function bits");
    check(static_cast<int>(c.baud + 0.5) == static_cast<int>(baud),
          what + ": baud detected as " + std::to_string(static_cast<int>(c.baud)));
    check(c.format == pocsag::Format::Kanji, what + ": format auto-detected as Kanji");
    check(c.text == text, what + ": text matches\n         got: " + c.text);
}

void test_alphanumeric() {
    const std::string text = "SHIYAKUSHO TEST 12345";
    std::vector<uint8_t> bytes(text.begin(), text.end());
    const std::vector<uint8_t> bits = bytes_to_bits_lsb_first(bytes);
    const auto tx = build_transmission(9998, 0, bits);
    const auto iq = modulate(tx, 48000.0, 1200.0, 0.0, 0.02, 7);

    const RunResult r = run_receiver(iq, pocsag::BaudMode::B1200,
                                     pocsag::Format::Alphanumeric,
                                     pocsag::KanjiByteOrder::Auto, 48000.0);
    check(!r.calls.empty() && r.calls.front().text == text,
          "Alphanumeric (JIS X 0201) round-trip" +
              (r.calls.empty() ? std::string(" — no call")
                               : " -> \"" + r.calls.front().text + "\""));
}

void test_numeric_over_air() {
    const std::string digits = "0123456789";
    const auto tx = build_transmission(4242, 0, numeric_to_bits(digits));
    const auto iq = modulate(tx, 48000.0, 512.0, -300.0, 0.02, 11);

    const RunResult r = run_receiver(iq, pocsag::BaudMode::Auto,
                                     pocsag::Format::Numeric,
                                     pocsag::KanjiByteOrder::Auto, 48000.0);
    check(!r.calls.empty() && r.calls.front().text == digits,
          "Numeric round-trip at 512 bps" +
              (r.calls.empty() ? std::string(" — no call")
                               : " -> \"" + r.calls.front().text + "\""));
}

void test_inverted_polarity() {
    const std::string text = "ヒナンシテクダサイ";
    const std::vector<uint8_t> bits =
        bytes_to_bits_lsb_first(utf8_to_sjis_bytes(text));
    auto tx = build_transmission(555, 1, bits);
    for (auto& b : tx) b ^= 1u;                       // spectrum-inverting front end

    const auto iq = modulate(tx, 48000.0, 1200.0, 0.0, 0.02, 13);
    const RunResult r = run_receiver(iq, pocsag::BaudMode::B1200,
                                     pocsag::Format::Auto,
                                     pocsag::KanjiByteOrder::Auto, 48000.0);
    check(!r.calls.empty() && r.calls.front().text == text &&
              r.calls.front().inverted,
          "Inverted polarity resolved from the sync codeword" +
              (r.calls.empty() ? std::string(" — no call")
                               : " -> \"" + r.calls.front().text + "\""));
}

// The numeric payload layout observed on the Karatsu broadcast address. The
// three positives are real off-air messages; the negatives check that the
// weekday/date cross-check and the range checks keep an unrelated numeric page
// from being labelled.
void test_interpret_numeric() {
    struct Case { const char* text; bool match; const char* what; };
    const Case cases[] = {
        {"00000001]30000826082700354****", true,  "off-air 2026-08-27 00:35 Thu"},
        {"00000001]30000826082700494****", true,  "off-air 2026-08-27 00:49 Thu"},
        {"00000001]30000826082701034****", true,  "off-air 2026-08-27 01:03 Thu"},
        {"00000001]30000826082701035****", false, "weekday contradicts the date"},
        {"00000001]30000826132701034****", false, "month 13"},
        {"00000001]30000826083201034****", false, "day 32"},
        {"00000001]30000826082725034****", false, "hour 25"},
        {"00000001]30000826082701634****", false, "minute 63"},
        {"00000001 30000826082701034****", false, "delimiter missing"},
        {"12345678]90123456789012345678",  false, "digits, no valid date"},
        {"0123456789",                     false, "too short"},
        {"",                               false, "empty"},
    };
    bool all = true;
    for (const Case& c : cases) {
        const bool got = !pocsag::interpret_numeric(c.text).empty();
        if (got != c.match) {
            all = false;
            check(false, std::string("interpret: ") + c.what);
        }
    }
    if (all) check(true, "Numeric layout recognised, and not over-matched (" +
                          std::to_string(sizeof(cases)/sizeof(cases[0])) + " cases)");

    const std::string r = pocsag::interpret_numeric("00000001]30000826082701034****");
    check(r == "Time broadcast: 2026-08-27 01:03 JST (Thu)",
          "Interpretation text: \"" + r + "\"");
}

} // namespace

int main() {
    std::printf("RCR STD-42 decoder self-test\n\n");

    std::printf("Coding (§3.4, §3.5)\n");
    test_bch();
    test_sync_words();

    std::printf("\nMessage formats (§3.6)\n");
    test_numeric_example();
    test_interpret_numeric();

    std::printf("\nEnd-to-end over synthetic RF\n");
    test_end_to_end("1200 bps, clean", 1200.0, pocsag::BaudMode::B1200, 0.0, 0.02, 1);
    test_end_to_end("1200 bps, auto baud", 1200.0, pocsag::BaudMode::Auto, 0.0, 0.02, 2);
    test_end_to_end("1200 bps, +1250 Hz offset", 1200.0, pocsag::BaudMode::Auto,
                    1250.0, 0.02, 3);
    test_end_to_end("512 bps, auto baud", 512.0, pocsag::BaudMode::Auto, 0.0, 0.02, 4);
    test_end_to_end("1200 bps, noisy", 1200.0, pocsag::BaudMode::B1200, -400.0, 0.35, 5);
    test_alphanumeric();
    test_numeric_over_air();
    test_inverted_polarity();

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
