#pragma once
// Best-effort reading of a numeric-format payload.
//
// RCR STD-42 §3.6.1 only defines how to turn 4-bit codes into characters; what
// the resulting digits mean is left entirely to the operator. Japanese
// municipal installations use the numeric format for machine-to-machine
// traffic, so a decoded call arrives as an undifferentiated digit string even
// though it is highly structured.
//
// This recognises one such layout, observed on the Karatsu 283.5365 MHz
// broadcast address (2097144), which sends a date/time/weekday every 14
// minutes:
//
//     00000001]30000826082701034****
//     |______|| |____||____||__||  |
//        (a)  (b)(c)   (d)   (e)(f) (g)
//
//     (a) 00000001  constant       (e) HHMM   time, JST
//     (b) ]         delimiter      (f) 4      day of week, Sunday = 0
//     (c) 300008    constant       (g) ****   padding (the reserved code 1010)
//     (d) YYMMDD    date
//
// Only (e) has been proven to vary — across three consecutive broadcasts it
// tracked the reception time to the minute — so the match is deliberately
// strict: every field is range-checked and the transmitted weekday must agree
// with the weekday computed from the transmitted date. An unrelated numeric
// page fails that and is left alone.
//
// This is a local convention, not part of the standard. Other municipalities
// may well differ.

#include <string>

namespace std42::pocsag {

// Returns a human-readable reading, or an empty string when the payload does
// not match a layout this knows about.
std::string interpret_numeric(const std::string& text);

} // namespace std42::pocsag
