#pragma once
// Serialisation of a decoded call, for the JSONL sinks and the plain-text log.
//
// The JSONL schema is documented in docs/JSONL_FORMAT.md. Text is emitted as
// UTF-8 directly, which is valid JSON; only the structural characters and C0
// controls are escaped.

#include <string>

#include "pocsag/receiver.h"

namespace std42::sink {

// One JSONL record (no trailing newline).
std::string serialize_call(const pocsag::DecodedCall& c, long long rx_time_ms);

// One human-readable log line, in the spirit of a paging monitor's text log.
std::string format_text_line(const pocsag::DecodedCall& c, long long rx_time_ms);

// "2026-08-26T14:40:12.345Z" for `ms` since the Unix epoch.
std::string iso8601_utc(long long ms);

} // namespace std42::sink
