#pragma once
// Records the VFO's complex baseband to a 32-bit float stereo WAV (I in the
// left channel, Q in the right) at the decoder's working rate.
//
// This exists for diagnosis. STD-42 leaves the bit rate unsignalled and real
// municipal installations do not always match the book, so when a signal
// demodulates but never frames, the only way to find out why is to capture the
// baseband and measure it — actual symbol rate, actual deviation, whether the
// sync codeword is present at all. The WAV layout is the same one SDR++'s own
// recorder writes, so the capture also opens in inspectrum, Audacity or
// SigDigger.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace std42::sink {

struct IqRecorderSnapshot {
    bool active = false;
    std::string path;
    long long frames = 0;      // complex samples written
    double seconds = 0.0;
    std::string error_message;
};

class IqRecorder {
public:
    IqRecorder(std::string path, double sample_rate);
    ~IqRecorder();

    // Called from the DSP thread; `data` is `count` interleaved I/Q pairs.
    void write(const float* interleaved, int count);
    void stop();

    IqRecorderSnapshot snapshot() const;

private:
    void write_header_locked(uint32_t data_bytes);

    mutable std::mutex gate_;
    std::string path_;
    double sample_rate_;
    std::FILE* fp_ = nullptr;
    long long frames_ = 0;
    std::string error_;
};

} // namespace std42::sink
