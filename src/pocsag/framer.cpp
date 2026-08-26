#include "pocsag/framer.h"

#include "pocsag/bch.h"

namespace std42::pocsag {

namespace {

inline int popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcount(v);
#else
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
#endif
}

inline bool matches_sync(uint32_t reg, int tolerance, bool& inverted) {
    if (popcount32(reg ^ kSyncCodeword) <= tolerance) { inverted = false; return true; }
    if (popcount32(reg ^ ~kSyncCodeword) <= tolerance) { inverted = true; return true; }
    return false;
}

} // namespace

Framer::Framer(MessageHandler on_message) : on_message_(std::move(on_message)) {
    bch_init();
}

void Framer::reset() {
    state_ = FramerState::Search;
    inverted_ = false;
    reg_ = 0;
    bits_in_word_ = 0;
    codeword_index_ = 0;
    preamble_run_ = 0;
    last_bit_ = -1;
    have_pending_ = false;
    pending_ = RawMessage{};
    cw_error_rate_ = 0.0;
}

void Framer::process_bit(int bit) {
    reg_ = (reg_ << 1) | (static_cast<uint32_t>(bit) & 1u);

    // Preamble is an unbroken 1010… alternation (§3.2); used only to drive the
    // "acquiring" indication in the UI.
    if (last_bit_ >= 0 && bit != last_bit_) {
        if (preamble_run_ < 1000) ++preamble_run_;
    } else {
        preamble_run_ = 0;
    }
    last_bit_ = bit;

    if (state_ == FramerState::Search) {
        bool inv = false;
        if (matches_sync(reg_, kSyncTolerance, inv)) {
            inverted_ = inv;
            state_ = FramerState::Locked;
            bits_in_word_ = 0;
            codeword_index_ = 0;
            ++stats_.batches;
        }
        return;
    }

    // Locked: accumulate whole codewords.
    if (++bits_in_word_ < 32) return;
    bits_in_word_ = 0;

    if (codeword_index_ < kCodewordsPerBatch) {
        handle_codeword(reg_, codeword_index_);
        ++codeword_index_;
        return;
    }

    // Codeword slot 16 must be the next batch's sync codeword.
    bool inv = false;
    if (matches_sync(reg_, kSyncTolerance, inv)) {
        inverted_ = inv;
        codeword_index_ = 0;
        ++stats_.batches;
    } else {
        ++stats_.sync_losses;
        emit_pending();
        state_ = FramerState::Search;
        codeword_index_ = 0;
    }
}

void Framer::handle_codeword(uint32_t cw, int index) {
    if (inverted_) cw = ~cw;

    uint32_t corrected = cw;
    const int nerr = bch_correct(corrected);

    const bool bad = (nerr < 0);
    cw_error_rate_ += ((bad ? 1.0 : 0.0) - cw_error_rate_) * 0.02;

    if (bad) {
        ++stats_.codewords_bad;
        if (have_pending_) ++pending_.bad_codewords;
        return;
    }
    if (nerr == 0) ++stats_.codewords_ok;
    else           ++stats_.codewords_corrected;

    const int frame = index / 2;

    // Flag bit (codeword bit 1) selects address vs message (§3.4).
    if ((corrected >> 31) & 1u) {
        // Message codeword: bits 2..21 are the payload, MSB first.
        if (!have_pending_) return;          // synced mid-call; nothing to attach to
        const uint32_t payload = (corrected >> 11) & 0xFFFFFu;
        for (int i = 19; i >= 0; --i) {
            pending_.bits.push_back(static_cast<uint8_t>((payload >> i) & 1u));
        }
        ++pending_.message_codewords;
        pending_.corrected_bits += nerr;
        return;
    }

    if (corrected == kIdleCodeword) {
        ++stats_.idles;
        emit_pending();                       // idle terminates the call (§3.4.4)
        return;
    }

    // Address codeword: a new call starts, so any previous one is complete.
    emit_pending();
    ++stats_.addresses;

    RawMessage m;
    // Bits 2..19 are the top 18 bits of the 21-bit address; the low 3 bits are
    // the frame number the codeword was transmitted in (§3.3).
    m.address = (((corrected >> 13) & 0x3FFFFu) << 3) | static_cast<uint32_t>(frame);
    m.function = static_cast<int>((corrected >> 11) & 0x3u);
    m.frame = frame;
    m.corrected_bits = nerr;

    pending_ = std::move(m);
    have_pending_ = true;
}

void Framer::emit_pending() {
    if (!have_pending_) return;
    have_pending_ = false;
    if (on_message_) on_message_(pending_);
    pending_ = RawMessage{};
}

void Framer::flush() { emit_pending(); }

} // namespace std42::pocsag
