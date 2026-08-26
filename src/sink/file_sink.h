#pragma once
// Append-mode line-oriented file sink.
//
// Each line is written and flushed to the C stream before write_line returns,
// so a reader tailing the file sees calls as they are decoded. There is no
// per-record fsync — a slow disk must not stall the DSP thread.

#include <cstdio>
#include <mutex>
#include <string>

#include "sink/sink.h"

namespace std42::sink {

struct FileSinkSnapshot {
    SinkPhase phase = SinkPhase::Off;
    std::string path;
    long long records_written = 0;
    long long bytes_written = 0;
    std::string error_message;
};

class FileLineSink : public IJsonlSink {
public:
    explicit FileLineSink(std::string path);
    ~FileLineSink() override;

    void write_line(const std::string& line) override;
    std::string status() const override;
    void stop() override;

    FileSinkSnapshot snapshot() const;

private:
    mutable std::mutex gate_;
    std::string path_;
    std::FILE* fp_ = nullptr;
    std::string status_;
    std::string error_;
    long long records_ = 0;
    long long bytes_ = 0;
};

} // namespace std42::sink
