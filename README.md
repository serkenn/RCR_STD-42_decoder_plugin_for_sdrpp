# RCR_STD-42_decoder_plugin_for_sdrpp

*日本語: [README.ja.md](README.ja.md)*

Out-of-tree decoder plugin for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)
that demodulates and decodes **RCR STD-42** — the ARIB standard for POCSAG radio
paging that Japanese municipal **同報系** (disaster-broadcast) systems are built
on. Calls are recovered straight from RF and rendered as text, including the
Shift-JIS kanji message format.

The waveform is an unencrypted, fully published 2-FSK paging signal:

| Item | Value | Reference |
|---|---|---|
| Modulation | 2-FSK, space = centre **+4.5 kHz**, mark = centre **−4.5 kHz** | §2.1.6 |
| Max deviation | ±5.0 kHz | §2.1.7 |
| Bit rate | **512 bps** / **1200 bps** | §1.1, §3.1 |
| Occupied bandwidth | ≤ 16 kHz, 25 kHz channel raster | §2.1.4, §2.1.2 |
| Band | 276.0125–283.9875 MHz, 320 channels | §2.2.1 |
| Multi-site offsets | 0, ±250, ±500, ±750, ±1000, ±1250 Hz | §2.1.8 |
| Preamble | ≥ 576 bits of `1010…` | §3.2 |
| Batch | sync codeword + 8 frames × 2 codewords = 17 codewords | §3.3 |
| Sync / idle codeword | `0x7CD215D8` / `0x7A89C197` | 図3.4-2, 図3.4-5 |
| FEC | BCH(31,21) + even parity, corrects ≤ 2 bits | §3.5 |
| Generator polynomial | x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1 (`0x769`) | §3.5.2 |
| Address | 21 bits; low 3 bits carried by the frame number | §3.3, §3.4.2 |
| Message formats | numeric (4 bit), JIS X 0201 (8 bit), Shift-JIS kanji (16 bit) | §3.6 |
| Encryption | none | — |

## Signal chain

```
VFO IQ (48 kHz, 20 kHz bandwidth)
 └─ quadrature FM discriminator                  → instantaneous frequency in Hz
 └─ per-baud slicing chain  (Auto runs 512 / 1200 / 2400 Bd side by side)
     ├─ Hamming-windowed low-pass  (cutoff 1.1 × R_b)
     ├─ centre + deviation tracking  (absorbs carrier offset, normalises ±4.5 kHz)
     ├─ zero-crossing DPLL symbol sync  (2nd order: phase and rate)
     └─ hard decision                            → bits
 └─ POCSAG framer
     ├─ sync-codeword search, ≤2-bit Hamming, against SC and its complement
     ├─ batch tracking  (SC + 8 frames × 2 codewords)
     ├─ BCH(31,21) + even parity  (syndrome table, ≤2-bit correction)
     └─ address / message / idle classification; address recombined with the
        frame number the address codeword arrived in
 └─ message decode  (each character field is bit-reversed — §3.6 sends LSB first)
     ├─ §3.6.1 numeric        4 bits/char via 図3.6-1
     ├─ §3.6.2 alphanumeric   8 bits/char, JIS X 0201 → UTF-8
     └─ §3.6.3 kanji         16 bits/char, Shift-JIS (CP932) → UTF-8
```

Two ambiguities the standard leaves open are resolved automatically rather than
being pushed onto the user:

- **Polarity.** A spectrum-inverting front end flips every bit. The framer
  matches the sync codeword against both `0x7CD215D8` and its complement and
  latches whichever fits, so no "invert" switch has to be found by trial.
- **Baud rate.** STD-42 defines both 512 and 1200 bps and signals neither
  on air. Rather than hunting one rate at a time and losing the start of a
  call, **Auto** runs a complete chain per candidate rate in parallel — each is
  only a short FIR plus a DPLL — and reports whichever achieves frame sync.

The message format is likewise not signalled. **Auto** decodes the payload under
all three readings and picks the one that maps cleanly, preferring the kanji
reading only when it actually yields double-byte characters (Shift-JIS is an
ASCII superset, so a plain-ASCII payload would otherwise be reported as kanji).
Any of these can be pinned manually in the panel.

### A note on the kanji byte order

§3.6.3 specifies that each kanji is converted to a **16-bit** Shift-JIS code and
transmitted LSB first, which places the trailing byte on the wire ahead of the
leading byte. Deployed equipment is not consistent about this — much of it sends
each *byte* LSB first in reading order instead. The decoder therefore tries both
and keeps whichever decodes without invalid characters; **Kanji bytes** in the
panel pins the choice to `Normal` or `Swapped` if a particular network needs it.

## Repository layout

```
.
├── .github/workflows/    # CI: self-test, macOS + Linux builds, tagged releases
├── cmake/toolchains/     # aarch64 CMake toolchain file
├── docs/                 # JSONL_FORMAT.md
├── external/SDRPlusPlus/ # SDR++ source (scripts/fetch-sdrpp.sh)
├── scripts/              # fetch-sdrpp.sh, gen_sjis_table.py
├── src/
│   ├── main.cpp          # SDR++ module + panel
│   ├── demod/            # FIR, FM discriminator, level tracking, symbol DPLL, FSK chain
│   ├── pocsag/           # BCH, batch framer, message formats, Shift-JIS, receiver
│   ├── sink/             # file / TCP JSONL sinks + call serializer
│   └── ui/               # eye diagram, sparkline, Japanese font loader
├── tests/                # end-to-end self-test (synthetic RF → text)
├── CMakeLists.txt
└── README.md
```

## Building

```sh
bash scripts/fetch-sdrpp.sh                       # SDR++ headers → external/
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The plugin needs only a C++17 compiler and the SDR++ **source tree** — not a
built SDR++. `sdrpp_core` symbols are resolved when SDR++ loads the module.
SDR++'s own headers include `<volk/volk.h>`, `<fftw3.h>` and `<GLFW/glfw3.h>`,
which are needed at compile time but never linked:

```sh
brew install volk fftw glfw                                   # macOS
sudo apt install libvolk-dev libfftw3-dev libglfw3-dev        # Ubuntu
```

On macOS the Homebrew include directory is discovered automatically.

### Self-test

```sh
bash tests/run.sh
```

This synthesises a full STD-42 transmission (preamble, batches, BCH-encoded
codewords), modulates it to complex baseband at ±4.5 kHz with a carrier offset
and noise, and checks that the receiver recovers the original Japanese text. It
also verifies the BCH decoder against every single- and double-bit error
pattern, the sync/idle codewords against 図3.4-2 / 図3.4-5, and the numeric bit
ordering against the worked "3681" example in 図3.4-4.

## Releases

GitHub Actions ([.github/workflows/build.yml](.github/workflows/build.yml)) runs
the self-test and builds the plugin for **macOS** (one universal binary covering
Apple Silicon and Intel) and Linux **x86_64** / **aarch64** on every push.
Pushing a `v*` tag attaches all three binaries to a GitHub Release:

```sh
git tag v0.1.0 && git push --tags
```

## Installing into SDR++

Copy the plugin into the SDR++ plugin directory, then enable it in the Module
Manager and create an instance. SDR++ scans that directory for a
platform-specific extension, so the file name matters:

| Platform | Directory | File |
|---|---|---|
| Linux | `/usr/lib/sdrpp/plugins/` | `rcr_std42_decoder.so` |
| macOS (app bundle) | `SDR++.app/Contents/Plugins/` | `rcr_std42_decoder.dylib` |

The macOS release asset is named `rcr_std42_decoder-macos-universal.dylib`;
rename it to `rcr_std42_decoder.dylib` when installing.

Tune the VFO onto a paging channel and the panel shows:

- a **status lamp** — searching / preamble / signal without frame sync / locked,
  with the detected bit rate and whether the polarity came out inverted;
- an **eye diagram** of the slicer input, folded over two symbol periods, with
  the decision threshold and the sampling instants marked — the direct analogue
  of a paging monitor's tuning display;
- **carrier offset**, **deviation** (a healthy signal reads near 4500 Hz per
  §2.1.6) and **quality** as 60-second sparklines;
- codeword statistics — accepted, BCH-corrected and rejected — plus batch,
  address and sync-loss counts;
- the **latest call** and a **Recent** list, rendered in Japanese.

**Address filter** takes a comma-separated list of decimal addresses; leave it
empty to record everything, which is the useful default on a municipal channel
where the address of interest is not known in advance.

### Japanese text rendering

SDR++ builds its font atlas from Roboto with only the default and Cyrillic glyph
ranges, so decoded Japanese would otherwise render as blank boxes. The plugin
adds a second font covering the Japanese ranges and re-uploads the atlas at
start-up. A font is auto-detected (Hiragino on macOS, Noto CJK / VL Gothic /
IPAGothic on Ubuntu); if none is found, install one —

```sh
sudo apt install fonts-noto-cjk                                # Ubuntu
```

— or set an explicit path in the panel. The ImGui font atlas can only be rebuilt
outside a frame, so a newly chosen path takes effect at the next SDR++ start.
Text written to the file and TCP sinks is always correct UTF-8 regardless of
what the panel can draw.

### Output

Every decoded call can be:

- appended to a **plain-text log** — one greppable line per call, in the spirit
  of a paging monitor's text output;
- streamed as **JSONL** to a **file** and/or a **TCP** port (127.0.0.1), with the
  text, the addressing, the per-call error counts and the raw payload bytes —
  see [docs/JSONL_FORMAT.md](docs/JSONL_FORMAT.md).

Paths default to the SDR++ config root (`~/.config/sdrpp/rcr_std42/`), and all
settings persist per instance in the SDR++ config.

## Platform support

| Target            | Supported | Release asset |
|-------------------|-----------|---------------|
| macOS arm64       | yes       | `…-macos-universal.dylib` |
| macOS x86_64      | yes       | `…-macos-universal.dylib` |
| Linux x86_64      | yes       | `…-linux-x86_64.so` |
| Linux aarch64     | yes       | `…-linux-aarch64.so` |
| Windows           | no        | — |

Windows is out of scope: a Windows plugin must link an MSVC-built SDR++ core,
so the unresolved-symbols approach used here does not apply.

## Scope and legality

RCR STD-42 is a published ARIB standard for an unencrypted paging service; this
plugin only demodulates and decodes what is already in the air. In Japan, the
secrecy of communications (電波法 §59) restricts using or disclosing the content
of transmissions not addressed to you. Municipal 同報系 broadcasts are intended
for public reception, but check what applies to the channel you are receiving.

## License

GNU Affero General Public License v3.0 — see [LICENSE](LICENSE).
