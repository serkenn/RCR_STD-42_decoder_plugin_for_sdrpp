// Replays a captured baseband WAV through the real receiver.
//
// This is how an off-air recording gets turned into a regression case: the
// decoder that runs inside SDR++ is exercised byte-for-byte, so what it
// reports here is what the panel would have shown.
//
//   c++ -std=c++17 -O2 -Isrc tests/replay.cpp src/demod/*.cpp src/pocsag/*.cpp \
//       -o build/replay && build/replay capture.wav

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pocsag/receiver.h"

using namespace std42;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: replay <capture.wav>\n"); return 2; }

    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("open"); return 1; }

    unsigned char hdr[44];
    if (std::fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { std::fprintf(stderr, "short file\n"); return 1; }
    const unsigned rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    std::printf("capture: %u Hz, %u ch, %u bit\n", rate, hdr[22] | (hdr[23] << 8), hdr[34] | (hdr[35] << 8));

    long long calls = 0;
    pocsag::Receiver rx(rate, [&](const pocsag::DecodedCall& c) {
        ++calls;
        std::printf("  CALL  addr=%u func=%c %d bps %-12s \"%s\"\n",
                    unsigned(c.address), char('A' + (c.function & 3)),
                    int(c.baud), pocsag::to_string(c.format), c.text.c_str());
    });
    rx.set_baud_mode(pocsag::BaudMode::Auto);
    rx.set_format(pocsag::Format::Auto);
    rx.set_kanji_byte_order(pocsag::KanjiByteOrder::Auto);

    std::vector<float> buf(8192 * 2);
    std::vector<demod::Complex32> iq(8192);
    long long total = 0;
    double next_report = 5.0;

    for (;;) {
        const size_t got = std::fread(buf.data(), sizeof(float), buf.size(), f);
        const int n = static_cast<int>(got / 2);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) iq[i] = demod::Complex32(buf[2*i], buf[2*i + 1]);
        rx.process(iq.data(), n);
        total += n;

        const double t = double(total) / rate;
        if (t >= next_report) {
            next_report += 5.0;
            std::printf("t=%6.1fs  idle=%-3s rate=%6.1f bps  reg=%.2f  dev=%5.0f Hz  "
                        "locked=%-3s batches=%lld\n",
                        t, rx.idle_pattern() ? "yes" : "no",
                        rx.idle_pattern_rate(), rx.idle_pattern_regularity(),
                        rx.deviation(), rx.locked() ? "yes" : "no",
                        rx.stats().batches);
        }
    }
    std::fclose(f);

    const auto s = rx.stats();
    std::printf("\n--- %.1f s replayed ---\n", double(total) / rate);
    std::printf("batches %lld   codewords %lld ok / %lld corrected / %lld bad\n",
                s.batches, s.codewords_ok, s.codewords_corrected, s.codewords_bad);
    std::printf("addresses %lld   sync losses %lld   calls decoded %lld\n",
                s.addresses, s.sync_losses, calls);
    return 0;
}
