#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/widgets/menu.h>
#include <module.h>
#include <signal_path/signal_path.h>
#include <signal_path/vfo_manager.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pocsag/receiver.h"
#include "sink/call_json.h"
#include "sink/file_sink.h"
#include "sink/iq_recorder.h"
#include "sink/tcp_sink.h"
#include "ui/eye_view.h"
#include "ui/jp_font.h"
#include "ui/sparkline.h"

namespace gui {
    extern Menu menu;
}

SDRPP_MOD_INFO{
    /* Name:            */ "rcr_std42_decoder",
    /* Description:     */ "RCR STD-42 (280 MHz POCSAG) municipal broadcast decoder",
    /* Author:          */ "serkenn",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

ConfigManager config;

namespace {

// STD-42 §2.1.4 caps the occupied bandwidth at 16 kHz and §2.1.2 puts channels
// on a 25 kHz raster, so a 20 kHz VFO passes the signal whole while rejecting
// the neighbours. 48 kHz of complex baseband leaves the ±4.5 kHz tones plus
// the ±1.25 kHz multi-site offsets of §2.1.8 well inside Nyquist, and gives
// 40 samples per symbol at 1200 bps for the timing loop to work with.
constexpr double kInputSampleRate = 48000.0;
constexpr double kVfoBandwidth = 20000.0;
constexpr double kVfoMinBandwidth = 8000.0;
constexpr double kVfoMaxBandwidth = 25000.0;

constexpr double kPushIntervalSec = 0.5;
constexpr size_t kRecentCalls = 64;

static_assert(sizeof(dsp::complex_t) == sizeof(std42::demod::Complex32),
              "SDR++ complex_t must be layout-compatible with std::complex<float>");

const char* const kBaudItems[] = {"Auto", "512 bps", "1200 bps", "2400 bps"};
const char* const kFormatItems[] = {"Auto", "Numeric", "Alphanumeric", "Kanji"};
const char* const kByteOrderItems[] = {"Auto", "Normal", "Swapped"};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

void copy_to_buf(const std::string& s, char* buf, size_t cap) {
    const size_t n = std::min(s.size(), cap - 1);
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

// Comma / space separated list of decimal addresses. Empty means "record
// everything", which is the useful default on a municipal channel where the
// interesting address is not known in advance.
std::unordered_set<uint32_t> parse_address_filter(const std::string& s) {
    std::unordered_set<uint32_t> out;
    uint64_t acc = 0;
    bool have = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            acc = acc * 10 + static_cast<uint64_t>(c - '0');
            if (acc > 0x1FFFFF) acc = 0x1FFFFF;      // 21-bit address space
            have = true;
        } else if (have) {
            out.insert(static_cast<uint32_t>(acc));
            acc = 0;
            have = false;
        }
    }
    if (have) out.insert(static_cast<uint32_t>(acc));
    return out;
}

// Compact summary retained for the panel; the full record goes to the sinks.
struct CallRecord {
    long long rx_time_ms = 0;
    uint32_t address = 0;
    int function = 0;
    double baud = 0.0;
    std::string format;
    std::string text;
    std::string interpretation;
    int payload_bytes = 0;
    int corrected_bits = 0;
    int bad_codewords = 0;
};

} // namespace

class Std42DecoderModule : public ModuleManager::Instance {
public:
    explicit Std42DecoderModule(std::string name)
        : name_(std::move(name)),
          eye_([this](std42::demod::EyePoint* dst, int cap) {
              return recv_ ? recv_->pull_eye(dst, cap) : 0;
          }) {
        const std::string default_dir = core::args["root"].s() + "/rcr_std42";

        config.acquire();
        if (!config.conf.contains(name_)) config.conf[name_] = json::object();
        auto& c = config.conf[name_];
        baud_mode_       = c.value("baudMode", 0);
        text_format_     = c.value("textFormat", 0);
        byte_order_      = c.value("kanjiByteOrder", 0);
        address_filter_  = c.value("addressFilter", std::string());
        text_log_enabled_ = c.value("textLogEnabled", false);
        text_log_path_   = c.value("textLogPath", default_dir + "/decoded.log");
        jsonl_enabled_   = c.value("jsonlFileEnabled", false);
        jsonl_path_      = c.value("jsonlFilePath", default_dir + "/decoded.jsonl");
        tcp_enabled_     = c.value("jsonlTcpEnabled", false);
        tcp_port_        = c.value("jsonlTcpPort", 7356);
        font_path_       = c.value("japaneseFontPath", std::string());
        iq_path_         = c.value("iqCapturePath", default_dir + "/capture.wav");
        config.release(true);

        copy_to_buf(text_log_path_, text_log_buf_, sizeof(text_log_buf_));
        copy_to_buf(jsonl_path_, jsonl_buf_, sizeof(jsonl_buf_));
        copy_to_buf(address_filter_, address_filter_buf_, sizeof(address_filter_buf_));
        copy_to_buf(font_path_, font_buf_, sizeof(font_buf_));
        copy_to_buf(iq_path_, iq_buf_, sizeof(iq_buf_));

        filter_set_ = parse_address_filter(address_filter_);

        if (text_log_enabled_ && !text_log_path_.empty())
            text_sink_ = std::make_unique<std42::sink::FileLineSink>(text_log_path_);
        if (jsonl_enabled_ && !jsonl_path_.empty())
            jsonl_sink_ = std::make_unique<std42::sink::FileLineSink>(jsonl_path_);
        if (tcp_enabled_ && tcp_port_ > 0)
            tcp_sink_ = std::make_unique<std42::sink::TcpJsonlSink>(tcp_port_);

        recv_ = std::make_unique<std42::pocsag::Receiver>(
            kInputSampleRate,
            [this](const std42::pocsag::DecodedCall& c2) { on_call(c2); });
        apply_decoder_settings();

        spark_offset_.set_range(-4000.0, 4000.0);
        spark_deviation_.set_range(0.0, 6000.0);
        spark_quality_.set_range(0.0, 100.0);

        last_push_ = std::chrono::steady_clock::now();

        vfo_ = sigpath::vfoManager.createVFO(
            name_, ImGui::WaterfallVFO::REF_CENTER, 0, kVfoBandwidth,
            kInputSampleRate, kVfoMinBandwidth, kVfoMaxBandwidth, false);
        sink_.init(vfo_->output, &Std42DecoderModule::iq_handler, this);
        sink_.start();

        gui::menu.registerEntry(name_, &Std42DecoderModule::menu_handler, this, this);
    }

    ~Std42DecoderModule() override {
        gui::menu.removeEntry(name_);
        if (enabled_) {
            sink_.stop();
            sigpath::vfoManager.deleteVFO(vfo_);
        }
        iq_recorder_.store(nullptr, std::memory_order_release);
        if (iq_owner_)   iq_owner_->stop();
        if (text_sink_)  text_sink_->stop();
        if (jsonl_sink_) jsonl_sink_->stop();
        if (tcp_sink_)   tcp_sink_->stop();
    }

    // SDR++ runs this at the end of MainWindow::init(), before the render loop
    // starts — the only point at which the ImGui font atlas may be rebuilt.
    void postInit() override { std42::ui::jp_font::init(font_path_); }

    void enable() override {
        if (enabled_) return;
        vfo_ = sigpath::vfoManager.createVFO(
            name_, ImGui::WaterfallVFO::REF_CENTER, 0, kVfoBandwidth,
            kInputSampleRate, kVfoMinBandwidth, kVfoMaxBandwidth, false);
        sink_.setInput(vfo_->output);
        sink_.start();
        enabled_ = true;
    }

    void disable() override {
        if (!enabled_) return;
        sink_.stop();
        sigpath::vfoManager.deleteVFO(vfo_);
        vfo_ = nullptr;
        enabled_ = false;
    }

    bool isEnabled() override { return enabled_; }

private:
    // ── DSP thread ────────────────────────────────────────────────────────
    static void iq_handler(dsp::complex_t* data, int count, void* ctx) {
        auto* self = static_cast<Std42DecoderModule*>(ctx);
        if (!self->enabled_ || !self->recv_) return;
        if (auto* rec = self->iq_recorder_.load(std::memory_order_acquire)) {
            rec->write(reinterpret_cast<const float*>(data), count);
        }
        self->recv_->process(
            reinterpret_cast<const std42::demod::Complex32*>(data), count);
    }

    // Invoked from the DSP thread, from inside Receiver::process(). Must not
    // call back into the receiver.
    void on_call(const std42::pocsag::DecodedCall& c) {
        {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            if (!filter_set_.empty() && !filter_set_.count(c.address)) {
                ++filtered_out_;
                return;
            }
        }

        const long long t = now_ms();

        if (jsonl_sink_ || tcp_sink_) {
            const std::string line = std42::sink::serialize_call(c, t);
            if (jsonl_sink_) jsonl_sink_->write_line(line);
            if (tcp_sink_)   tcp_sink_->write_line(line);
        }
        if (text_sink_) {
            text_sink_->write_line(std42::sink::format_text_line(c, t));
        }

        CallRecord r;
        r.rx_time_ms = t;
        r.address = c.address;
        r.function = c.function;
        r.baud = c.baud;
        r.format = std42::pocsag::to_string(c.format);
        r.text = c.text;
        r.interpretation = c.interpretation;
        r.payload_bytes = static_cast<int>(c.raw_bytes.size());
        r.corrected_bits = c.corrected_bits;
        r.bad_codewords = c.bad_codewords;

        std::lock_guard<std::mutex> lk(ui_mtx_);
        recent_.push_front(std::move(r));
        while (recent_.size() > kRecentCalls) recent_.pop_back();
    }

    void apply_decoder_settings() {
        using namespace std42::pocsag;
        static const BaudMode kBaud[] = {BaudMode::Auto, BaudMode::B512,
                                         BaudMode::B1200, BaudMode::B2400};
        static const Format kFmt[] = {Format::Auto, Format::Numeric,
                                      Format::Alphanumeric, Format::Kanji};
        static const KanjiByteOrder kOrder[] = {KanjiByteOrder::Auto,
                                                KanjiByteOrder::Normal,
                                                KanjiByteOrder::Swapped};
        recv_->set_baud_mode(kBaud[std::clamp(baud_mode_, 0, 3)]);
        recv_->set_format(kFmt[std::clamp(text_format_, 0, 3)]);
        recv_->set_kanji_byte_order(kOrder[std::clamp(byte_order_, 0, 2)]);
    }

    template <class T>
    void save_field(const char* key, const T& value) {
        config.acquire();
        config.conf[name_][key] = value;
        config.release(true);
    }

    // ── GUI ───────────────────────────────────────────────────────────────
    static void menu_handler(void* ctx) {
        static_cast<Std42DecoderModule*>(ctx)->draw_menu();
    }

    void draw_menu() {
        if (!enabled_) style::beginDisabled();

        draw_status();
        ImGui::Separator();
        draw_signal();
        ImGui::Separator();
        draw_decoder_settings();
        ImGui::Separator();
        draw_statistics();
        ImGui::Separator();
        draw_latest();
        ImGui::Separator();
        draw_output();

        if (!enabled_) style::endDisabled();
    }

    void draw_status() {
        const bool locked = recv_ && recv_->locked();
        const bool preamble = recv_ && recv_->preamble();
        const float dev = recv_ ? recv_->deviation() : 0.0f;

        ImU32 lamp;
        std::string label;
        if (!enabled_) {
            lamp = IM_COL32(120, 120, 120, 255);
            label = "Disabled";
        } else if (locked) {
            lamp = IM_COL32(70, 180, 85, 255);
            char b[64];
            std::snprintf(b, sizeof(b), "Locked - %d bps%s",
                          static_cast<int>(recv_->active_baud() + 0.5),
                          recv_->inverted() ? " (inverted)" : "");
            label = b;
        } else if (preamble) {
            lamp = IM_COL32(220, 170, 40, 255);
            label = "Preamble - acquiring";
        } else if (recv_ && recv_->idle_pattern()) {
            // Not a fault: the channel is up and carrying its between-calls
            // idle pattern. Green, because there is nothing to fix.
            lamp = IM_COL32(70, 180, 85, 255);
            char b[80];
            std::snprintf(b, sizeof(b), "Channel idle - %d bps 1010 pattern",
                          static_cast<int>(recv_->idle_pattern_rate() + 0.5));
            label = b;
        } else if (dev > 1500.0f) {
            lamp = IM_COL32(220, 170, 40, 255);
            label = "Signal, no frame sync";
        } else {
            lamp = IM_COL32(200, 70, 70, 255);
            label = "Searching";
        }

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + 7, p.y + 9), 5.0f, lamp);
        ImGui::Dummy(ImVec2(16, 0));
        ImGui::SameLine();
        ImGui::TextUnformatted(label.c_str());
    }

    void draw_signal() {
        const auto now = std::chrono::steady_clock::now();
        const double offset = recv_ ? recv_->carrier_offset() : 0.0;
        const double dev = recv_ ? recv_->deviation() : 0.0;
        const double quality = recv_ ? recv_->quality() * 100.0 : 0.0;

        if (std::chrono::duration<double>(now - last_push_).count() >= kPushIntervalSec) {
            spark_offset_.push(offset);
            spark_deviation_.push(dev);
            spark_quality_.push(quality);
            last_push_ = now;
        }

        const float avail = ImGui::GetContentRegionAvail().x;
        eye_.draw(avail, std::min(std::max(avail * 0.45f, 70.0f), 130.0f));

        char buf[64];
        const float cap_w = ImGui::CalcTextSize("Carrier offset").x + 10.0f;

        std::snprintf(buf, sizeof(buf), "%+.0f Hz", offset);
        spark_offset_.draw("Carrier offset", buf, IM_COL32(90, 160, 230, 255), cap_w);

        // §2.1.6 specifies ±4.5 kHz, so a healthy signal reads near 4500.
        const ImU32 dev_col = (dev > 3000.0 && dev < 6000.0)
                                  ? IM_COL32(70, 180, 85, 255)
                                  : IM_COL32(220, 170, 40, 255);
        std::snprintf(buf, sizeof(buf), "%.0f Hz", dev);
        spark_deviation_.draw("Deviation", buf, dev_col, cap_w);

        const ImU32 q_col = quality > 90.0 ? IM_COL32(70, 180, 85, 255)
                          : quality > 50.0 ? IM_COL32(220, 170, 40, 255)
                                           : IM_COL32(200, 70, 70, 255);
        // Say what the number is derived from; "0 %" on an idle channel used
        // to read as a fault rather than as "nothing is being sent".
        const char* q_src = !recv_               ? ""
                          : recv_->locked()      ? " codeword"
                          : recv_->idle_pattern() ? " idle"
                                                  : "";
        std::snprintf(buf, sizeof(buf), "%.0f %%%s", quality, q_src);
        spark_quality_.draw("Quality", buf, q_col, cap_w);

        ImGui::TextDisabled("Eye opening %.1f   symbol clock %.1f Bd",
                            eye_.opening(),
                            recv_ ? recv_->recovered_baud() : 0.0);
        if (recv_ && recv_->idle_pattern()) {
            // Surface the measurement, so an unexpected idle rate is visible
            // rather than looking like a decoder failure.
            ImGui::TextDisabled("Channel activity: %.0f bps 1010 pattern "
                                "(regularity %.2f) - not POCSAG",
                                recv_->idle_pattern_rate(),
                                recv_->idle_pattern_regularity());
        }
    }

    void draw_decoder_settings() {
        ImGui::LeftLabel("Baud rate");
        ImGui::FillWidth();
        if (ImGui::Combo(("##baud_" + name_).c_str(), &baud_mode_,
                         kBaudItems, IM_ARRAYSIZE(kBaudItems))) {
            apply_decoder_settings();
            save_field("baudMode", baud_mode_);
        }

        ImGui::LeftLabel("Text format");
        ImGui::FillWidth();
        if (ImGui::Combo(("##fmt_" + name_).c_str(), &text_format_,
                         kFormatItems, IM_ARRAYSIZE(kFormatItems))) {
            apply_decoder_settings();
            save_field("textFormat", text_format_);
        }

        // §3.6.3 transmits each kanji as a 16-bit code LSB first, which puts
        // the Shift-JIS trailing byte first; deployed equipment varies, so the
        // order is selectable.
        ImGui::LeftLabel("Kanji bytes");
        ImGui::FillWidth();
        if (ImGui::Combo(("##order_" + name_).c_str(), &byte_order_,
                         kByteOrderItems, IM_ARRAYSIZE(kByteOrderItems))) {
            apply_decoder_settings();
            save_field("kanjiByteOrder", byte_order_);
        }

        ImGui::TextUnformatted("Address filter");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint(("##addr_" + name_).c_str(),
                                     "all addresses", address_filter_buf_,
                                     sizeof(address_filter_buf_),
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            address_filter_ = address_filter_buf_;
            filter_set_ = parse_address_filter(address_filter_);
            save_field("addressFilter", address_filter_);
        }
        {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            if (!filter_set_.empty()) {
                ImGui::TextDisabled("%zu address(es); %lld call(s) filtered out",
                                    filter_set_.size(), filtered_out_);
            }
        }
    }

    void draw_statistics() {
        if (!recv_) return;
        const auto s = recv_->stats();
        ImGui::Text("Symbols: %lld   Batches: %lld", recv_->symbols(), s.batches);
        ImGui::Text("Codewords: %lld ok / %lld corrected / %lld bad",
                    s.codewords_ok, s.codewords_corrected, s.codewords_bad);
        ImGui::Text("Addresses: %lld   Sync losses: %lld", s.addresses, s.sync_losses);
        ImGui::Text("Calls decoded: %lld", recv_->calls());
    }

    void draw_latest() {
        ImGui::TextUnformatted("Latest call");

        CallRecord latest;
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(ui_mtx_);
            count = recent_.size();
            if (!recent_.empty()) latest = recent_.front();
        }

        if (count == 0) {
            ImGui::TextDisabled("(none yet)");
            return;
        }

        ImGui::TextDisabled("%s  addr %u  func %c  %d bps  %s",
                            std42::sink::iso8601_utc(latest.rx_time_ms).c_str(),
                            static_cast<unsigned>(latest.address),
                            static_cast<char>('A' + (latest.function & 3)),
                            static_cast<int>(latest.baud),
                            latest.format.c_str());
        if (latest.bad_codewords > 0) {
            ImGui::TextColored(ImVec4(0.86f, 0.67f, 0.16f, 1.0f),
                               "%d codeword(s) lost - message may be incomplete",
                               latest.bad_codewords);
        }

        // A recognised layout is what the operator meant; the raw characters
        // stay visible underneath so nothing is hidden behind a guess.
        if (latest.format == "Binary") {
            ImGui::TextDisabled("%d bytes of data, not a text message",
                                latest.payload_bytes);
        }
        if (!latest.interpretation.empty()) {
            ImGui::TextColored(ImVec4(0.44f, 0.78f, 0.53f, 1.0f), "%s",
                               latest.interpretation.c_str());
        }
        {
            std42::ui::jp_font::Scoped jp;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextWrapped("%s", latest.text.c_str());
            ImGui::PopTextWrapPos();
        }

        if (ImGui::TreeNode(("Recent (" + std::to_string(count) + ")##recent_" + name_).c_str())) {
            std::deque<CallRecord> snapshot;
            {
                std::lock_guard<std::mutex> lk(ui_mtx_);
                snapshot = recent_;
            }
            std42::ui::jp_font::Scoped jp;
            for (const auto& r : snapshot) {
                ImGui::TextWrapped("[%u] %s", static_cast<unsigned>(r.address),
                                   r.interpretation.empty() ? r.text.c_str()
                                                            : r.interpretation.c_str());
            }
            ImGui::TreePop();
        }
    }

    void draw_output() {
        ImGui::TextUnformatted("Output");

        // Plain-text log.
        bool text_on = static_cast<bool>(text_sink_);
        if (ImGui::Checkbox(("Text log##textlog_" + name_).c_str(), &text_on)) {
            if (text_on && !text_log_path_.empty()) {
                text_sink_ = std::make_unique<std42::sink::FileLineSink>(text_log_path_);
            } else {
                if (text_sink_) { text_sink_->stop(); text_sink_.reset(); }
                text_on = false;
            }
            text_log_enabled_ = text_on;
            save_field("textLogEnabled", text_log_enabled_);
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText(("##textpath_" + name_).c_str(), text_log_buf_,
                             sizeof(text_log_buf_),
                             ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            text_log_path_ = text_log_buf_;
            save_field("textLogPath", text_log_path_);
        }
        if (text_sink_) {
            const auto s = text_sink_->snapshot();
            ImGui::TextDisabled("%lld line(s)", s.records_written);
        }

        // JSONL file.
        bool jsonl_on = static_cast<bool>(jsonl_sink_);
        if (ImGui::Checkbox(("JSONL file##jsonl_" + name_).c_str(), &jsonl_on)) {
            if (jsonl_on && !jsonl_path_.empty()) {
                jsonl_sink_ = std::make_unique<std42::sink::FileLineSink>(jsonl_path_);
            } else {
                if (jsonl_sink_) { jsonl_sink_->stop(); jsonl_sink_.reset(); }
                jsonl_on = false;
            }
            jsonl_enabled_ = jsonl_on;
            save_field("jsonlFileEnabled", jsonl_enabled_);
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText(("##jsonlpath_" + name_).c_str(), jsonl_buf_,
                             sizeof(jsonl_buf_),
                             ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            jsonl_path_ = jsonl_buf_;
            save_field("jsonlFilePath", jsonl_path_);
        }
        if (jsonl_sink_) {
            const auto s = jsonl_sink_->snapshot();
            if (s.phase == std42::sink::SinkPhase::Error) {
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "%s",
                                   s.error_message.c_str());
            } else {
                ImGui::TextDisabled("%lld record(s)", s.records_written);
            }
        }

        // JSONL over TCP.
        bool tcp_on = static_cast<bool>(tcp_sink_);
        if (ImGui::Checkbox(("JSONL TCP##tcp_" + name_).c_str(), &tcp_on)) {
            if (tcp_on) {
                tcp_sink_ = std::make_unique<std42::sink::TcpJsonlSink>(tcp_port_);
            } else {
                if (tcp_sink_) { tcp_sink_->stop(); tcp_sink_.reset(); }
            }
            tcp_enabled_ = tcp_on;
            save_field("jsonlTcpEnabled", tcp_enabled_);
        }
        ImGui::LeftLabel("TCP port");
        ImGui::FillWidth();
        if (ImGui::InputInt(("##tcpport_" + name_).c_str(), &tcp_port_, 0)) {
            tcp_port_ = std::clamp(tcp_port_, 1, 65535);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) save_field("jsonlTcpPort", tcp_port_);
        if (tcp_sink_) {
            const auto s = tcp_sink_->snapshot();
            if (s.phase == std42::sink::SinkPhase::Error) {
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "%s",
                                   s.error_message.c_str());
            } else {
                ImGui::TextDisabled("127.0.0.1:%d - %d client(s), %lld sent",
                                    s.port, s.client_count, s.records_sent);
            }
        }

        // Baseband capture, for working out why a signal demodulates but never
        // frames. Written at the decoder's own 48 kHz working rate.
        ImGui::Separator();
        bool rec_on = iq_recorder_.load(std::memory_order_acquire) != nullptr;
        if (ImGui::Checkbox(("Record IQ (48 kHz WAV)##iqrec_" + name_).c_str(), &rec_on)) {
            if (rec_on && !iq_path_.empty()) {
                auto rec = std::make_unique<std42::sink::IqRecorder>(
                    iq_path_, kInputSampleRate);
                iq_recorder_.store(rec.get(), std::memory_order_release);
                iq_owner_ = std::move(rec);
            } else {
                iq_recorder_.store(nullptr, std::memory_order_release);
                if (iq_owner_) { iq_owner_->stop(); iq_owner_.reset(); }
            }
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText(("##iqpath_" + name_).c_str(), iq_buf_, sizeof(iq_buf_),
                             ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            iq_path_ = iq_buf_;
            save_field("iqCapturePath", iq_path_);
        }
        if (iq_owner_) {
            const auto s2 = iq_owner_->snapshot();
            if (!s2.error_message.empty()) {
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "%s",
                                   s2.error_message.c_str());
            } else {
                ImGui::TextDisabled("%.1f s captured (%lld samples)",
                                    s2.seconds, s2.frames);
            }
        }

        // Japanese font. The ImGui atlas can only be rebuilt before the render
        // loop starts, so a new path is picked up at the next start.
        ImGui::Separator();
        ImGui::TextUnformatted("Japanese font");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint(("##font_" + name_).c_str(),
                                     "auto-detect", font_buf_, sizeof(font_buf_),
                                     ImGuiInputTextFlags_EnterReturnsTrue) ||
            ImGui::IsItemDeactivatedAfterEdit()) {
            font_path_ = font_buf_;
            save_field("japaneseFontPath", font_path_);
        }
        if (std42::ui::jp_font::available()) {
            ImGui::TextDisabled("loaded: %s",
                                std42::ui::jp_font::loaded_path().c_str());
        } else {
            // Not an error in the usual case: adding the module at runtime
            // simply defers the font to the next start.
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", std42::ui::jp_font::error().c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled("Font changes apply at the next SDR++ start.");
    }

    // ── Members ───────────────────────────────────────────────────────────
    std::string name_;
    bool enabled_ = true;

    VFOManager::VFO* vfo_ = nullptr;
    dsp::sink::Handler<dsp::complex_t> sink_;

    std::unique_ptr<std42::pocsag::Receiver> recv_;
    std::unique_ptr<std42::sink::FileLineSink> text_sink_;
    std::unique_ptr<std42::sink::FileLineSink> jsonl_sink_;
    std::unique_ptr<std42::sink::TcpJsonlSink> tcp_sink_;

    // Published to the DSP thread with release/acquire so a capture can be
    // started and stopped without locking the audio path.
    std::unique_ptr<std42::sink::IqRecorder> iq_owner_;
    std::atomic<std42::sink::IqRecorder*> iq_recorder_{nullptr};

    std42::ui::EyeView eye_;
    std42::ui::Sparkline spark_offset_;
    std42::ui::Sparkline spark_deviation_;
    std42::ui::Sparkline spark_quality_;

    int baud_mode_ = 0;
    int text_format_ = 0;
    int byte_order_ = 0;
    std::string address_filter_;
    std::string text_log_path_;
    std::string jsonl_path_;
    std::string font_path_;
    std::string iq_path_;
    int tcp_port_ = 7356;
    bool text_log_enabled_ = false;
    bool jsonl_enabled_ = false;
    bool tcp_enabled_ = false;

    char address_filter_buf_[256] = {};
    char text_log_buf_[512] = {};
    char jsonl_buf_[512] = {};
    char font_buf_[512] = {};
    char iq_buf_[512] = {};

    std::mutex ui_mtx_;
    std::deque<CallRecord> recent_;
    std::unordered_set<uint32_t> filter_set_;
    long long filtered_out_ = 0;

    std::chrono::steady_clock::time_point last_push_{};
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/rcr_std42_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new Std42DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete static_cast<Std42DecoderModule*>(instance);
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
