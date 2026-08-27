#include "sink/call_json.h"

#include <cstdio>
#include <ctime>

namespace std42::sink {

namespace {

// UTF-8 bytes pass through verbatim — they are valid inside a JSON string.
void append_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void append_str(std::string& out, const char* key, const std::string& v) {
    out += ",\"";
    out += key;
    out += "\":";
    append_escaped(out, v);
}

void append_num(std::string& out, const char* key, long long v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    out += ",\"";
    out += key;
    out += "\":";
    out += buf;
}

void append_bool(std::string& out, const char* key, bool v) {
    out += ",\"";
    out += key;
    out += "\":";
    out += (v ? "true" : "false");
}

void append_hex(std::string& out, const char* key,
                const std::vector<uint8_t>& data) {
    static const char* H = "0123456789abcdef";
    out += ",\"";
    out += key;
    out += "\":\"";
    for (uint8_t b : data) {
        out.push_back(H[b >> 4]);
        out.push_back(H[b & 0x0F]);
    }
    out.push_back('"');
}

// Function bits 00/01/10/11 select functions A..D (§3.4.2).
char function_label(int f) { return static_cast<char>('A' + (f & 3)); }

} // namespace

std::string iso8601_utc(long long ms) {
    const std::time_t secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms % 1000));
    return buf;
}

std::string serialize_call(const pocsag::DecodedCall& c, long long rx_time_ms) {
    std::string out;
    out.reserve(c.text.size() + c.raw_bytes.size() * 2 + 384);

    // Opens with rx_time_ms so every later field can prefix its own comma.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", rx_time_ms);
    out += "{\"rx_time_ms\":";
    out += buf;

    append_str(out, "rx_time", iso8601_utc(rx_time_ms));

    append_num(out, "address", static_cast<long long>(c.address));
    append_num(out, "function", c.function);
    append_str(out, "function_label", std::string(1, function_label(c.function)));
    append_num(out, "frame", c.frame);

    append_num(out, "baud", static_cast<long long>(c.baud));
    append_bool(out, "inverted", c.inverted);

    append_str(out, "format", pocsag::to_string(c.format));
    if (c.format == pocsag::Format::Kanji) {
        append_str(out, "byte_order", pocsag::to_string(c.byte_order));
    }
    append_str(out, "text", c.text);
    if (!c.interpretation.empty()) append_str(out, "interpretation", c.interpretation);
    append_num(out, "chars", c.chars);
    append_num(out, "double_byte", c.double_byte);
    append_num(out, "invalid", c.invalid);
    if (c.header_bytes > 0) append_num(out, "header_bytes", c.header_bytes);

    append_num(out, "message_codewords", c.message_codewords);
    append_num(out, "corrected_bits", c.corrected_bits);
    append_num(out, "bad_codewords", c.bad_codewords);
    append_num(out, "payload_bits", c.payload_bits);
    append_hex(out, "raw_hex", c.raw_bytes);

    out.push_back('}');
    return out;
}

std::string format_text_line(const pocsag::DecodedCall& c, long long rx_time_ms) {
    char head[160];
    std::snprintf(head, sizeof(head),
                  "%s  addr=%u func=%c frame=%d  %d bps  %s%s  ",
                  iso8601_utc(rx_time_ms).c_str(),
                  static_cast<unsigned>(c.address),
                  function_label(c.function),
                  c.frame,
                  static_cast<int>(c.baud),
                  pocsag::to_string(c.format),
                  c.bad_codewords > 0 ? " [partial]" : "");

    std::string line(head);
    if (c.format == pocsag::Format::Binary) {
        char n[48];
        std::snprintf(n, sizeof(n), "%zu bytes of data (see raw_hex)",
                      c.raw_bytes.size());
        return line + n;
    }
    if (!c.interpretation.empty()) {
        line += c.interpretation;
        line += "  | ";
    }
    // Keep the record on one line so the log stays greppable.
    for (char ch : c.text) {
        if (ch == '\n' || ch == '\r') line += "\\n";
        else line.push_back(ch);
    }
    return line;
}

} // namespace std42::sink
