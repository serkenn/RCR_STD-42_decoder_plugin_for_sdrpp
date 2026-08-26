#pragma once
// POCSAG batch framer, per RCR STD-42 §3.1-§3.4.
//
// Frame structure (図3.1-1):
//
//   preamble (>=576 bits of 1010…) | batch | batch | …
//   batch = sync codeword + 8 frames, one frame = 2 codewords  => 17 codewords
//
// The sync codeword (図3.4-2) is 0x7CD215D8 and the idle codeword (図3.4-5) is
// 0x7A89C197. Codeword bit 1 is the flag bit: 0 marks an address codeword, 1 a
// message codeword. The receiver's 21-bit address has its low 3 bits carried
// implicitly by the frame number the address codeword appears in (§3.3), so
// they are recombined here.
//
// Sync is matched with a small Hamming tolerance, and against both the sync
// codeword and its complement, so a spectrum-inverting front end decodes
// without the user having to know.

#include <cstdint>
#include <functional>
#include <vector>

namespace std42::pocsag {

inline constexpr uint32_t kSyncCodeword = 0x7CD215D8u;
inline constexpr uint32_t kIdleCodeword = 0x7A89C197u;
inline constexpr int kCodewordsPerBatch = 16;   // excluding the sync codeword

enum class FramerState { Search, Locked };

struct RawMessage {
    uint32_t address = 0;         // full 21-bit address
    int function = 0;             // 0..3 == function A..D
    int frame = 0;                // 0..7, the frame the address appeared in
    std::vector<uint8_t> bits;    // message bits, transmission order
    int message_codewords = 0;
    int corrected_bits = 0;       // BCH corrections across the whole call
    int bad_codewords = 0;        // codewords dropped as uncorrectable
};

struct FramerStats {
    long long batches = 0;
    long long codewords_ok = 0;       // accepted with no correction
    long long codewords_corrected = 0;
    long long codewords_bad = 0;      // uncorrectable
    long long sync_losses = 0;
    long long addresses = 0;
    long long idles = 0;
};

class Framer {
public:
    using MessageHandler = std::function<void(const RawMessage&)>;

    explicit Framer(MessageHandler on_message = nullptr);

    void set_handler(MessageHandler h) { on_message_ = std::move(h); }

    void process_bit(int bit);
    // Ends any call in progress, e.g. when the squelch closes.
    void flush();
    void reset();

    FramerState state() const { return state_; }
    bool inverted() const { return inverted_; }
    const FramerStats& stats() const { return stats_; }
    // EWMA of the per-codeword rejection rate, 0..1.
    double codeword_error_rate() const { return cw_error_rate_; }
    // True while the input looks like the 1010… preamble (§3.2).
    bool preamble() const { return preamble_run_ >= kPreambleRun; }

private:
    static constexpr int kSyncTolerance = 2;   // d=5 leaves room for two errors
    static constexpr int kPreambleRun = 24;

    void handle_codeword(uint32_t cw, int index);
    void emit_pending();

    MessageHandler on_message_;

    FramerState state_ = FramerState::Search;
    bool inverted_ = false;

    uint32_t reg_ = 0;        // 32-bit sliding window
    int bits_in_word_ = 0;    // bits accumulated toward the current codeword
    int codeword_index_ = 0;  // 0..15 within the batch, 16 == expecting sync
    int preamble_run_ = 0;
    int last_bit_ = -1;

    bool have_pending_ = false;
    RawMessage pending_;

    FramerStats stats_;
    double cw_error_rate_ = 0.0;
};

} // namespace std42::pocsag
