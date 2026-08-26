#include "pocsag/interpret.h"

#include <cstdio>

namespace std42::pocsag {

namespace {

bool digits(const std::string& s, size_t pos, size_t n, int& out) {
    if (pos + n > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// Sakamoto's method. Returns 0..6 with Sunday = 0.
int day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y/4 - y/100 + y/400 + t[m - 1] + d) % 7;
}

bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap(y)) return 29;
    return d[m - 1];
}

const char* const kWeekday[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

} // namespace

std::string interpret_numeric(const std::string& text) {
    // The layout is fixed-width and delimiter-anchored.
    if (text.size() < 26 || text[8] != ']') return {};

    int yy, mm, dd, hh, mi, wd;
    if (!digits(text, 15, 2, yy)) return {};
    if (!digits(text, 17, 2, mm)) return {};
    if (!digits(text, 19, 2, dd)) return {};
    if (!digits(text, 21, 2, hh)) return {};
    if (!digits(text, 23, 2, mi)) return {};
    if (!digits(text, 25, 1, wd)) return {};

    const int year = 2000 + yy;
    if (mm < 1 || mm > 12) return {};
    if (dd < 1 || dd > days_in_month(year, mm)) return {};
    if (hh > 23 || mi > 59 || wd > 6) return {};

    // The clincher: a coincidental digit string will not have a weekday that
    // agrees with its own date.
    if (day_of_week(year, mm, dd) != wd) return {};

    char buf[80];
    std::snprintf(buf, sizeof(buf), "Time broadcast: %04d-%02d-%02d %02d:%02d JST (%s)",
                  year, mm, dd, hh, mi, kWeekday[wd]);
    return buf;
}

} // namespace std42::pocsag
