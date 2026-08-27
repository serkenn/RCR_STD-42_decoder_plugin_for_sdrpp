#include "pocsag/receiver.h"

#include "pocsag/bch.h"
#include "pocsag/interpret.h"

#include <algorithm>

namespace std42::pocsag {

const char* to_string(BaudMode m) {
    switch (m) {
        case BaudMode::B512:  return "512 bps";
        case BaudMode::B1200: return "1200 bps";
        case BaudMode::B2400: return "2400 bps";
        case BaudMode::Auto:
        default:              return "Auto";
    }
}

std::vector<double> baud_candidates(BaudMode mode) {
    switch (mode) {
        case BaudMode::B512:  return {512.0};
        case BaudMode::B1200: return {1200.0};
        case BaudMode::B2400: return {2400.0};
        case BaudMode::Auto:
        default:
            // 512 and 1200 are the rates STD-42 §1.1 defines; 2400 is included
            // because POCSAG equipment on adjacent services uses it and it
            // costs almost nothing to watch for.
            return {512.0, 1200.0, 2400.0};
    }
}

Receiver::Receiver(double sample_rate, Handler on_call)
    : sample_rate_(sample_rate), on_call_(std::move(on_call)) {
    bch_init();
    fm_.configure(sample_rate_);
    pattern_.configure(sample_rate_);
    std::lock_guard<std::mutex> lk(gate_);
    rebuild_chains_locked();
}

void Receiver::rebuild_chains_locked() {
    const std::vector<double> rates = baud_candidates(baud_mode_);
    chains_.clear();
    chains_.reserve(rates.size());
    for (size_t i = 0; i < rates.size(); ++i) {
        auto c = std::make_unique<Chain>();
        c->chain.configure(sample_rate_, rates[i]);
        c->framer = std::make_unique<Framer>(
            [this, i](const RawMessage& m) { on_raw_message(m, i); });
        chains_.push_back(std::move(c));
    }
    active_.store(0, std::memory_order_relaxed);
}

void Receiver::set_baud_mode(BaudMode mode) {
    std::lock_guard<std::mutex> lk(gate_);
    if (baud_mode_ == mode) return;
    baud_mode_ = mode;
    rebuild_chains_locked();
}

void Receiver::set_format(Format f) {
    std::lock_guard<std::mutex> lk(gate_);
    format_ = f;
}

void Receiver::set_kanji_byte_order(KanjiByteOrder o) {
    std::lock_guard<std::mutex> lk(gate_);
    byte_order_ = o;
}

void Receiver::process(const demod::Complex32* data, int count) {
    std::lock_guard<std::mutex> lk(gate_);
    if (chains_.empty()) return;

    for (int i = 0; i < count; ++i) {
        // The discriminator is baud-independent, so it runs once for all
        // candidate chains.
        const float hz = fm_.process(data[i]);
        pattern_.process(hz);
        for (auto& c : chains_) {
            const int bit = c->chain.process(hz);
            if (bit >= 0) c->framer->process_bit(bit);
        }
    }

    // Prefer a chain that currently holds frame sync for the UI readout.
    for (size_t i = 0; i < chains_.size(); ++i) {
        if (chains_[i]->framer->state() == FramerState::Locked) {
            active_.store(static_cast<int>(i), std::memory_order_relaxed);
            break;
        }
    }
}

void Receiver::on_raw_message(const RawMessage& m, size_t chain_index) {
    // Called with gate_ held, from process().
    active_.store(static_cast<int>(chain_index), std::memory_order_relaxed);

    DecodedCall call;
    call.address = m.address;
    call.function = m.function;
    call.frame = m.frame;
    call.baud = chains_[chain_index]->chain.baud();
    call.inverted = chains_[chain_index]->framer->inverted();
    call.message_codewords = m.message_codewords;
    call.corrected_bits = m.corrected_bits;
    call.bad_codewords = m.bad_codewords;
    call.payload_bits = static_cast<int>(m.bits.size());
    call.raw_bytes = bits_to_bytes(m.bits);

    const DecodedText d = decode_message(m.bits, format_, byte_order_);
    call.format = d.format;
    call.byte_order = d.byte_order;
    call.text = d.text;
    call.chars = d.chars;
    call.double_byte = d.double_byte;
    call.invalid = d.invalid;
    call.header_bytes = d.header_bytes;
    if (call.format == Format::Numeric) {
        call.interpretation = interpret_numeric(call.text);
    }

    calls_.fetch_add(1, std::memory_order_relaxed);
    if (on_call_) on_call_(call);
}

// ── Metrics ───────────────────────────────────────────────────────────────
// Each accessor takes the same lock as process(); the UI reads them a handful
// of times per frame, so the contention is irrelevant.

double Receiver::active_baud() const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return 0.0;
    return chains_[static_cast<size_t>(i)]->chain.baud();
}

bool Receiver::locked() const {
    std::lock_guard<std::mutex> lk(gate_);
    for (const auto& c : chains_) {
        if (c->framer->state() == FramerState::Locked) return true;
    }
    return false;
}

bool Receiver::preamble() const {
    std::lock_guard<std::mutex> lk(gate_);
    for (const auto& c : chains_) {
        if (c->framer->preamble()) return true;
    }
    return false;
}

bool Receiver::inverted() const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return false;
    return chains_[static_cast<size_t>(i)]->framer->inverted();
}

float Receiver::carrier_offset() const {
    std::lock_guard<std::mutex> lk(gate_);
    if (chains_.empty()) return 0.0f;
    // The centre estimate is baud-independent; chain 0 is as good as any.
    return chains_.front()->chain.carrier_offset();
}

float Receiver::deviation() const {
    std::lock_guard<std::mutex> lk(gate_);
    if (chains_.empty()) return 0.0f;
    const int i = active_.load(std::memory_order_relaxed);
    const size_t idx = (i >= 0 && static_cast<size_t>(i) < chains_.size())
                           ? static_cast<size_t>(i) : 0;
    return chains_[idx]->chain.deviation();
}

double Receiver::recovered_baud() const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return 0.0;
    return chains_[static_cast<size_t>(i)]->chain.recovered_baud();
}

double Receiver::timing_error() const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return 0.0;
    return chains_[static_cast<size_t>(i)]->chain.timing_error();
}

double Receiver::codeword_error_rate() const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return 0.0;
    return chains_[static_cast<size_t>(i)]->framer->codeword_error_rate();
}

bool Receiver::idle_pattern() const {
    std::lock_guard<std::mutex> lk(gate_);
    return pattern_.is_idle_pattern();
}

double Receiver::idle_pattern_rate() const {
    std::lock_guard<std::mutex> lk(gate_);
    return pattern_.pattern_rate();
}

double Receiver::idle_pattern_regularity() const {
    std::lock_guard<std::mutex> lk(gate_);
    return pattern_.regularity();
}

double Receiver::quality() const {
    std::lock_guard<std::mutex> lk(gate_);
    if (chains_.empty()) return 0.0;
    const int i = active_.load(std::memory_order_relaxed);
    const size_t idx = (i >= 0 && static_cast<size_t>(i) < chains_.size())
                           ? static_cast<size_t>(i) : 0;
    const Chain& c = *chains_[idx];

    if (c.framer->state() == FramerState::Locked) {
        // Framed: the rejected-codeword rate is the authoritative measure.
        return std::clamp(1.0 - c.framer->codeword_error_rate(), 0.0, 1.0);
    }

    // Not framed. Reporting 0 here was actively misleading: a municipal
    // channel is idle almost all of the time, so a perfectly clean, strong
    // signal read "Quality 0 %" and looked like a reception problem. What the
    // user needs between calls is how cleanly the bit stream is being
    // recovered, and the regularity of the idle pattern's zero crossings
    // measures exactly that — it degrades with noise the same way.
    //
    // pattern_ is read directly rather than through is_idle_pattern(), which
    // would take gate_ again; it is already held here.
    if (pattern_.is_idle_pattern()) {
        return std::clamp(pattern_.regularity(), 0.0, 1.0);
    }
    return c.framer->preamble() ? 0.25 : 0.0;
}

long long Receiver::symbols() const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return 0;
    return chains_[static_cast<size_t>(i)]->chain.symbols();
}

FramerStats Receiver::stats() const {
    std::lock_guard<std::mutex> lk(gate_);
    FramerStats total;
    for (const auto& c : chains_) {
        const FramerStats& s = c->framer->stats();
        total.batches += s.batches;
        total.codewords_ok += s.codewords_ok;
        total.codewords_corrected += s.codewords_corrected;
        total.codewords_bad += s.codewords_bad;
        total.sync_losses += s.sync_losses;
        total.addresses += s.addresses;
        total.idles += s.idles;
    }
    return total;
}

int Receiver::pull_eye(demod::EyePoint* dst, int capacity) const {
    std::lock_guard<std::mutex> lk(gate_);
    const int i = active_.load(std::memory_order_relaxed);
    if (i < 0 || static_cast<size_t>(i) >= chains_.size()) return 0;
    return chains_[static_cast<size_t>(i)]->chain.pull_eye(dst, capacity);
}

} // namespace std42::pocsag
