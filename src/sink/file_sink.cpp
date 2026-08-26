#include "sink/file_sink.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <utility>

namespace std42::sink {

const char* to_string(SinkPhase p) {
    switch (p) {
        case SinkPhase::Active: return "Active";
        case SinkPhase::Error:  return "Error";
        case SinkPhase::Off:
        default:                return "Off";
    }
}

FileLineSink::FileLineSink(std::string path) : path_(std::move(path)) {
    std::error_code ec;
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    fp_ = std::fopen(path_.c_str(), "ab");
    if (fp_) {
        std::setvbuf(fp_, nullptr, _IOFBF, 1 << 16);
        status_ = "writing -> " + p.filename().string();
    } else {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
    }
}

FileLineSink::~FileLineSink() { stop(); }

void FileLineSink::write_line(const std::string& line) {
    if (line.empty()) return;
    std::lock_guard<std::mutex> lk(gate_);
    if (!fp_) return;

    const size_t n = std::fwrite(line.data(), 1, line.size(), fp_);
    const bool nl_ok = std::fputc('\n', fp_) != EOF;
    const bool flushed = std::fflush(fp_) == 0;

    if (n != line.size() || !nl_ok || !flushed) {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
        std::fclose(fp_);
        fp_ = nullptr;
        return;
    }
    bytes_ += static_cast<long long>(line.size() + 1);
    ++records_;
}

std::string FileLineSink::status() const {
    std::lock_guard<std::mutex> lk(gate_);
    return status_;
}

void FileLineSink::stop() {
    std::lock_guard<std::mutex> lk(gate_);
    if (fp_) {
        std::fflush(fp_);
        std::fclose(fp_);
        fp_ = nullptr;
    }
    status_ = "off";
}

FileSinkSnapshot FileLineSink::snapshot() const {
    std::lock_guard<std::mutex> lk(gate_);
    FileSinkSnapshot s;
    s.path = path_;
    s.records_written = records_;
    s.bytes_written = bytes_;
    s.error_message = error_;
    if (!error_.empty()) s.phase = SinkPhase::Error;
    else if (fp_)        s.phase = SinkPhase::Active;
    else                 s.phase = SinkPhase::Off;
    return s;
}

} // namespace std42::sink
