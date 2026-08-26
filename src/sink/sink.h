#pragma once
// Common interface for the JSONL output sinks.
//
// Every implementation must be safe to call from the DSP thread and must never
// block it: a sink whose consumer has stalled drops records rather than
// applying back-pressure to the decoder.

#include <string>

namespace std42::sink {

enum class SinkPhase { Off, Active, Error };

const char* to_string(SinkPhase p);

class IJsonlSink {
public:
    virtual ~IJsonlSink() = default;

    // One JSONL record, without the trailing newline; the sink appends it.
    virtual void write_line(const std::string& line) = 0;
    virtual std::string status() const = 0;
    virtual void stop() = 0;
};

} // namespace std42::sink
