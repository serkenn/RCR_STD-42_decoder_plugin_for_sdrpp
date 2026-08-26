# JSONL output format

*日本語: [JSONL_FORMAT.ja.md](JSONL_FORMAT.ja.md)*

The STD-42 decoder plugin can stream decoded calls as
[JSON Lines](https://jsonlines.org/) (JSONL) to two sinks:

- **File** — appended to the configured path, flushed after every line.
- **TCP** — broadcast to every client connected to the configured port on
  `127.0.0.1`.

Both sinks emit the **same line content**. Each sink appends a single `\n` after
each record; the record itself never contains a raw newline (any newline in the
payload is escaped — see [Encoding](#encoding)).

The serializer is [`src/sink/call_json.cpp`](../src/sink/call_json.cpp)
(`serialize_call`).

## When a line is emitted

One line is emitted **per call** — that is, each time an address codeword's
message is complete. A call ends when the next address codeword arrives, when an
idle codeword arrives (RCR STD-42 §3.4.4), or when frame sync is lost.

- An address codeword with no message codewords following it still produces a
  line, with `text` empty and `payload_bits` zero — a tone-only page.
- Idle codewords themselves never produce a line; they are only counted in the
  panel.
- A call that lost codewords to uncorrectable errors is still emitted, with
  `bad_codewords` greater than zero. Treat its `text` as partial.
- When an **address filter** is configured in the panel, calls to other
  addresses produce no line at all.

## Record schema

Each line is a single JSON object. Field order is stable, but consumers should
not rely on it.

| Field               | Type    | Always? | Description |
|---------------------|---------|---------|-------------|
| `rx_time_ms`        | number  | yes     | Receiver wall-clock at decode time, milliseconds since the Unix epoch (UTC). |
| `rx_time`           | string  | yes     | The same instant as ISO-8601 UTC, `YYYY-MM-DDThh:mm:ss.sssZ`. |
| `address`           | number  | yes     | Full 21-bit receiver address, 0–2097151. Recombined from the 18 bits in the codeword and the frame number the codeword arrived in (§3.3). |
| `function`          | number  | yes     | Function bits, 0–3 (§3.4.2). |
| `function_label`    | string  | yes     | The same value as `"A"`–`"D"`. |
| `frame`             | number  | yes     | Frame 0–7 the address codeword occupied. Equals `address & 7`. |
| `baud`              | number  | yes     | Bit rate the call was decoded at: `512`, `1200` or `2400`. |
| `inverted`          | boolean | yes     | `true` if the sync codeword matched the complement, i.e. the signal path inverts polarity. |
| `format`            | string  | yes     | Message format actually used: `"Numeric"`, `"Alphanumeric"` or `"Kanji"`. Never `"Auto"` — this is the resolved choice. |
| `byte_order`        | string  | kanji   | `"Normal"` or `"Swapped"` — which Shift-JIS byte order decoded cleanly. Present **only when `format` is `"Kanji"`**. |
| `text`              | string  | yes     | The decoded message as UTF-8. Empty for a tone-only page. |
| `chars`             | number  | yes     | Characters produced by the decoder, including any replacement characters. |
| `double_byte`       | number  | yes     | Of those, how many were double-byte Shift-JIS. Always 0 for the numeric and alphanumeric formats. |
| `invalid`           | number  | yes     | Characters that could not be mapped and were emitted as U+FFFD. A non-zero value on an otherwise clean call usually means the wrong format or byte order was pinned manually. |
| `message_codewords` | number  | yes     | Message codewords accepted for this call. |
| `corrected_bits`    | number  | yes     | Total bits repaired by BCH across the call's codewords. A rising value is the earliest sign of a marginal signal. |
| `bad_codewords`     | number  | yes     | Codewords dropped as uncorrectable (more than 2 bit errors). Non-zero means `text` has a gap. |
| `payload_bits`      | number  | yes     | Raw message bits collected, i.e. `20 × message_codewords`. |
| `raw_hex`           | string  | yes     | The payload packed into bytes exactly as §3.6 specifies (each character field LSB first), lower-case hex, no separators. Trailing bits that do not fill a byte are dropped. Empty string for a tone-only page. |

`raw_hex` is the undecoded payload: for the kanji format it is the Shift-JIS
byte stream **before** any byte-order correction, so a consumer that disagrees
with the plugin's format or byte-order choice can re-decode from it. In the
kanji example below, `raw_hex` starts `82b1 82bf 82e7` — Shift-JIS for
こ・ち・ら.

## Example

A kanji broadcast, wrapped here for readability — the real record is one line:

```json
{"rx_time_ms":1774523412345,"rx_time":"2026-03-26T14:30:12.345Z",
 "address":1234567,"function":3,"function_label":"D","frame":7,
 "baud":1200,"inverted":false,
 "format":"Kanji","byte_order":"Normal",
 "text":"こちらは防災行政無線です。ただいま試験放送を行っています。",
 "chars":29,"double_byte":28,"invalid":0,
 "message_codewords":24,"corrected_bits":2,"bad_codewords":0,
 "payload_bits":480,
 "raw_hex":"82b182bf82e782cd96688dd08d7390ad96b390fc82c582b7814282bd82be82a282dc8e8e8cb195fa919782f08d7382c182c482a282dc82b78142"}
```

A numeric page:

```json
{"rx_time_ms":1774523500000,"rx_time":"2026-03-26T14:31:40.000Z",
 "address":4242,"function":0,"function_label":"A","frame":2,
 "baud":512,"inverted":false,
 "format":"Numeric","text":"0123456789",
 "chars":10,"double_byte":0,"invalid":0,
 "message_codewords":2,"corrected_bits":0,"bad_codewords":0,
 "payload_bits":40,"raw_hex":"1032547698"}
```

## Encoding

Records are UTF-8. Text is emitted as UTF-8 bytes directly, which is valid
inside a JSON string; only `"`, `\`, and the C0 control characters are escaped
(`\n`, `\r`, `\t`, and `\u00XX` for the rest). No non-ASCII character is
`\u`-escaped, so a consumer must read the stream as UTF-8 rather than ASCII.

## Consuming the stream

```sh
# File sink
tail -f ~/.config/sdrpp/rcr_std42/decoded.jsonl | jq -r '"\(.rx_time) [\(.address)] \(.text)"'

# TCP sink
nc 127.0.0.1 7356 | jq -c 'select(.bad_codewords == 0) | {rx_time, address, text}'
```

The TCP sink queues up to 2048 records per client and drops the oldest when a
client stops reading, so a stalled consumer never blocks the decoder or the
other clients. It listens on the loopback interface only.
