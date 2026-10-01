# Protocols

Every wire format in the engine follows a published Nasdaq specification. Offsets and lengths
below were checked field by field against the official documents:

* Nasdaq TotalView-ITCH 5.0 (market data)
* Nasdaq OUCH 4.2 (order entry)
* MoldUDP64 v1.00 (UDP transport for ITCH)

All integers on the wire are big-endian. Prices are unsigned 32-bit with four implied decimals
(`1234500` = 123.4500) and are held as signed 64-bit `Price` inside the engine so that
`price * quantity` cannot overflow.

## ITCH 5.0 (`include/optitrade/itch/`)

Offsets are from the first byte of the message (the message type).

| Type | Message | Length | Supported |
|---|---|---:|---|
| `S` | System Event | 12 | decoded, no book effect |
| `R` | Stock Directory | 39 | decoded, registers symbol and locate |
| `A` | Add Order | 36 | applied to the book |
| `F` | Add Order with MPID | 40 | applied to the book |
| `E` | Order Executed | 31 | applied to the book |
| `C` | Order Executed With Price | 36 | applied to the book |
| `X` | Order Cancel (partial) | 23 | applied to the book |
| `D` | Order Delete | 19 | applied to the book |
| `U` | Order Replace | 35 | applied to the book |
| `P` | Trade (non-cross) | 44 | decoded, records last trade price |

Other ITCH message types (trading actions, cross trades, NOII, ...) are counted and skipped by the
stream decoder; they are not errors. Common header: type @0, stock locate (u16) @1, tracking number
(u16) @3, timestamp (48-bit nanoseconds since midnight) @5.

Framing for files follows the Nasdaq sample files (`BinaryFILE`): a 2-byte big-endian length before
every message. `itch::decode_stream` handles partial trailing frames, so it can be fed from a socket or
a file in arbitrary chunks.

Decoder contract: it never reads past the span it is given, rejects a wrong length
(`truncated` / `bad_length`), an invalid side or zero share count (`bad_field`) and reports unsupported
types as `unknown_type`.

## OUCH 4.2 (`include/optitrade/ouch/`)

| Direction | Type | Message | Length |
|---|---|---|---:|
| to exchange | `O` | Enter Order | 49 |
| to exchange | `U` | Replace Order | 47 |
| to exchange | `X` | Cancel Order | 19 |
| from exchange | `A` | Accepted | 66 |
| from exchange | `U` | Replaced | 80 |
| from exchange | `C` | Canceled | 28 |
| from exchange | `E` | Executed | 40 |
| from exchange | `J` | Rejected | 24 |

Order tokens are 14 characters; the engine generates them as zero-padded decimal ids. Other outbound
types (Broken Trade, AIQ Cancel, ...) decode as `unknown_type`.

Stream framing is a plain 2-byte length prefix. This is a deliberate simplification: production OUCH
sessions run inside SoupBinTCP, which this project does not implement.

## MoldUDP64 (`include/optitrade/net/mold64.hpp`)

20-byte header: session (10 bytes), sequence number of the first message (u64), message count (u16),
followed by `count` blocks of `length (u16) + payload`. Count 0 is a heartbeat, `0xFFFF` is end of
session. `for_each_message` validates the whole packet (block lengths, count, trailing bytes) before
delivering anything. `SequenceTracker` reports gaps (with the number of missing messages), duplicates and
session changes, and heartbeats can reveal a gap too.

Loss handling stops at detection: there is no retransmission request, so after a gap the engine cancels
its orders and stops trading (`Engine::on_feed_gap`).

## Capture file (`include/optitrade/replay/capture.hpp`)

`OTCAP001`: 16-byte header (magic, u32 version = 1, u32 reserved), then records of
`u64 timestamp (LE)`, `u16 length (LE)`, payload (one unframed ITCH message). Maximum payload 4096 bytes.
Truncated files, bad magic, wrong versions and oversized or empty records are reported through
`error()`; a file that ends on a record boundary is a clean end. `ItchFileReader` reads Nasdaq
BinaryFILE files directly and takes timestamps from the messages themselves.
