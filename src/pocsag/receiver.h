#pragma once
// Top-level STD-42 receiver: IQ in, decoded calls out.
//
//   IQ → FM discriminator → [ per-baud slicing chain → POCSAG framer ] → text
//
// STD-42 §1.1 defines both 512 bps and 1200 bps systems, and the baud rate is
// not signalled anywhere in the transmission. Rather than hunting one rate at
// a time and missing the start of a call, "Auto" runs a complete chain per
// candidate rate in parallel — each is only a short FIR plus a DPLL — and
// reports whichever one achieves frame sync.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "demod/fm_demod.h"
#include "demod/fsk_chain.h"
#include "demod/pattern_detector.h"
#include "demod/types.h"
#include "pocsag/framer.h"
#include "pocsag/message.h"

namespace std42::pocsag {

enum class BaudMode { Auto, B512, B1200, B2400 };

const char* to_string(BaudMode m);
// Candidate rates for `mode`; Auto returns all of them.
std::vector<double> baud_candidates(BaudMode mode);

struct DecodedCall {
    // ── Addressing ────────────────────────────────────────────────────────
    uint32_t address = 0;        // 21-bit receiver address (§3.3)
    int function = 0;            // 0..3 == function A..D (§3.4.2)
    int frame = 0;               // 0..7

    // ── Link ──────────────────────────────────────────────────────────────
    double baud = 0.0;
    bool inverted = false;       // polarity resolved from the sync codeword

    // ── Payload ───────────────────────────────────────────────────────────
    Format format = Format::Numeric;
    KanjiByteOrder byte_order = KanjiByteOrder::Normal;
    std::string text;            // UTF-8
    // Human reading of a recognised operator-specific layout; empty when the
    // payload matches none. See pocsag/interpret.h.
    std::string interpretation;
    int chars = 0;
    int double_byte = 0;
    int invalid = 0;

    // ── Quality ───────────────────────────────────────────────────────────
    int message_codewords = 0;
    int corrected_bits = 0;
    int bad_codewords = 0;

    // ── Raw ───────────────────────────────────────────────────────────────
    std::vector<uint8_t> raw_bytes;   // payload packed LSB-first per §3.6
    int payload_bits = 0;
};

class Receiver {
public:
    using Handler = std::function<void(const DecodedCall&)>;

    Receiver(double sample_rate, Handler on_call);

    // Called from the DSP thread with one VFO block.
    void process(const demod::Complex32* data, int count);

    // ── Configuration (UI thread) ─────────────────────────────────────────
    void set_baud_mode(BaudMode mode);
    void set_format(Format f);
    void set_kanji_byte_order(KanjiByteOrder o);

    BaudMode baud_mode() const { return baud_mode_; }
    Format format() const { return format_; }
    KanjiByteOrder kanji_byte_order() const { return byte_order_; }

    // ── Metrics (UI thread) ───────────────────────────────────────────────
    // Index of the chain currently worth displaying: the locked one, else the
    // one that most recently decoded, else the first.
    int active_index() const { return active_.load(std::memory_order_relaxed); }
    double active_baud() const;
    bool locked() const;
    bool preamble() const;
    bool inverted() const;

    float carrier_offset() const;     // Hz
    float deviation() const;          // Hz, mean |deviation|
    double recovered_baud() const;    // Bd
    double timing_error() const;      // symbol periods
    double codeword_error_rate() const;

    // Channel activity that is not POCSAG: many municipal transmitters idle
    // with a continuous 1010… pattern between calls.
    bool idle_pattern() const;
    double idle_pattern_rate() const;      // bps implied by the pattern
    double idle_pattern_regularity() const;
    // 0..1 reception quality, in the spirit of the reference decoder's
    // percentage readout: 1.0 means locked with no rejected codewords.
    double quality() const;

    long long symbols() const;
    FramerStats stats() const;
    long long calls() const { return calls_.load(std::memory_order_relaxed); }

    int pull_eye(demod::EyePoint* dst, int capacity) const;

private:
    struct Chain {
        demod::FskChain chain;
        std::unique_ptr<Framer> framer;
    };

    void rebuild_chains_locked();
    void on_raw_message(const RawMessage& m, size_t chain_index);

    const double sample_rate_;
    Handler on_call_;

    mutable std::mutex gate_;         // guards chains_ against reconfiguration
    demod::FmDemod fm_;
    demod::PatternDetector pattern_;
    std::vector<std::unique_ptr<Chain>> chains_;

    BaudMode baud_mode_ = BaudMode::Auto;
    Format format_ = Format::Auto;
    KanjiByteOrder byte_order_ = KanjiByteOrder::Auto;

    std::atomic<int> active_{0};
    std::atomic<long long> calls_{0};
};

} // namespace std42::pocsag
