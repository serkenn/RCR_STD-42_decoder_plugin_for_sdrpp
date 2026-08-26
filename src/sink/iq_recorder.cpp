#include "sink/iq_recorder.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <utility>

namespace std42::sink {

namespace {

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
void put_u16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}

constexpr int kChannels = 2;          // I, Q
constexpr int kBitsPerSample = 32;    // IEEE float

} // namespace

IqRecorder::IqRecorder(std::string path, double sample_rate)
    : path_(std::move(path)), sample_rate_(sample_rate) {
    std::error_code ec;
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    fp_ = std::fopen(path_.c_str(), "wb");
    if (!fp_) {
        error_ = std::strerror(errno);
        return;
    }
    std::setvbuf(fp_, nullptr, _IOFBF, 1 << 18);
    write_header_locked(0);           // patched with the real size on stop()
}

IqRecorder::~IqRecorder() { stop(); }

// Canonical 44-byte WAV header, format 3 (IEEE float).
void IqRecorder::write_header_locked(uint32_t data_bytes) {
    uint8_t h[44];
    std::memcpy(h + 0, "RIFF", 4);
    put_u32(h + 4, 36u + data_bytes);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    put_u32(h + 16, 16);                                   // fmt chunk size
    put_u16(h + 20, 3);                                    // WAVE_FORMAT_IEEE_FLOAT
    put_u16(h + 22, kChannels);
    put_u32(h + 24, static_cast<uint32_t>(sample_rate_));
    put_u32(h + 28, static_cast<uint32_t>(sample_rate_) * kChannels * (kBitsPerSample / 8));
    put_u16(h + 32, kChannels * (kBitsPerSample / 8));     // block align
    put_u16(h + 34, kBitsPerSample);
    std::memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);

    std::fseek(fp_, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), fp_);
}

void IqRecorder::write(const float* interleaved, int count) {
    if (count <= 0) return;
    std::lock_guard<std::mutex> lk(gate_);
    if (!fp_) return;

    const size_t floats = static_cast<size_t>(count) * 2;
    if (std::fwrite(interleaved, sizeof(float), floats, fp_) != floats) {
        error_ = std::strerror(errno);
        std::fclose(fp_);
        fp_ = nullptr;
        return;
    }
    frames_ += count;
}

void IqRecorder::stop() {
    std::lock_guard<std::mutex> lk(gate_);
    if (!fp_) return;
    const uint32_t data_bytes =
        static_cast<uint32_t>(frames_ * kChannels * (kBitsPerSample / 8));
    write_header_locked(data_bytes);
    std::fflush(fp_);
    std::fclose(fp_);
    fp_ = nullptr;
}

IqRecorderSnapshot IqRecorder::snapshot() const {
    std::lock_guard<std::mutex> lk(gate_);
    IqRecorderSnapshot s;
    s.active = (fp_ != nullptr);
    s.path = path_;
    s.frames = frames_;
    s.seconds = (sample_rate_ > 0.0) ? frames_ / sample_rate_ : 0.0;
    s.error_message = error_;
    return s;
}

} // namespace std42::sink
