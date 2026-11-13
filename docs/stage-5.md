# Stage 5 brief: market data

Stages 1 to 4 built a compressor for text. Stage 5 points the same parts at
a different kind of data: a day of the NASDAQ TotalView-ITCH 5.0 feed, a
few hundred million small binary messages that add, cancel and execute
orders. A general-purpose compressor sees bytes. This stage's codec parses
the messages, keeps the state they describe (which orders are alive, in
which stock, at what price) and codes each field as its difference from
what that state expects. A cancel names an order by a 64-bit number; to a
model that holds the order, it is "the fourth most recent order in this
stock", and the order's shares and price need no bits at all.

The codec is new; the engine is not. Every bit is still predicted by
history tables, state maps, a mixer and an APM from stages 2 and 3, and
coded by stage 1's coder. What changes is where the contexts come from.

**Exit number:** on `corpus/12302019.NASDAQ_ITCH50` (8,251,407,909 bytes,
268,744,780 messages), the `itch` archive is at most **4.00 bytes per
message**, and at least **45% smaller than the fair baseline**: every
field split into its own column, delta-coded where that helps, each
column compressed with `zstd -19`, which measures 8.04 bytes per message.
Expect about 3.9. A prototype of this design made 1,043,686,913 bytes,
3.884 per message and 51.7% under the fair baseline, and decoded all of
it back to the file. A result outside 3.85 to 3.95 means the code departs
from the brief; find out where before tuning anything.

The comparison is the deliverable, more than the number: one table, from a
script anyone can rerun, with every baseline in bytes per message and MB/s.
[Measurements](#measurements-behind-the-choices) has the prototype's
version of it.

## Scope

The roadmap splits the stage into three parts, each useful alone. They land
in this order, and may be three pull requests.

**5a. Schema, benchmark and baselines.**

1. The ITCH 5.0 schema as a `constexpr` table: every message type, its
   length and its fields, from the specification.
2. `cpress-bench` counts messages and reports bytes per message and MB/s.
3. The baselines, as a script and one small tool: gzip, zstd, xz, `mix9`,
   and the fair baseline.

**5b. The model.**

4. A field coder: symbols and numbers through six context inputs.
5. The market state: per-stock state, a bounded order table, the
   reference counters.
6. The message model: each field's expectation and contexts.
7. Codec id 4, `itch`, with a memory level. Not the default.
8. An escape, so that any bytes round-trip, ITCH or not.

**5c. Streaming.**

9. File-to-file compression and decompression for codec 4 that never hold
   the uncompressed file in memory.

And throughout: tests, and results recorded in the README.

Out of scope, each measured or considered and left for the stage named:

- An order book, or any part of one. The roadmap planned to code a new
  order's price against the best bid and ask. Measured, that is worth
  0.5% over coding the price as it is in the context of its stock
  ([Left for later](#left-for-later)), and the stage does not need it.
- Threads and independent blocks. Issue #4 left the decision to this
  brief, and it is no: an order message refers to an order added minutes
  or hours earlier, so a block that starts without the order table codes
  every reference in full. Speed is recorded, not tuned.
- Prefetching for codec 4, as stage 4 did for `mix9`. The encoder knows a
  whole message before it codes it, so the same trick applies; it is not
  needed for the exit and is not in the prototype.
- Other feeds, other ITCH versions, the SoupBinTCP and MoldUDP64 framings.
  They round-trip through the escape and compress badly.
- Choosing the codec by looking at the input. `cpress c --codec itch` asks
  for it; `mix9` stays the default.
- Streaming for codecs 1 to 3, and streaming the payload into the decoder
  (section 8 says what is held instead).
- Any change to codecs 1 to 3, to the container header, or to the parts of
  stages 1 to 4 that codec 4 reuses: `HistoryTable`, `ProbabilityMap`,
  `Mixer`, `Apm`, `stretch`, `squash`, the coder, `TableArray`.
- The ratio-against-speed plot and the write-up (stage 6).

## Files

```
src/itch/schema.hpp           message types, lengths and fields, from the specification
src/itch/field_coder.hpp      FieldCoder: symbols and numbers through six context inputs
src/itch/residual.hpp         wrapping, signed and stepped differences, and their inverses
src/itch/order_table.hpp      OrderTable: live orders by reference, bounded; each stock's list
src/itch/market.hpp           Market: everything the messages so far imply
src/itch/message_model.hpp    MessageModel: codes one message, either direction
src/codec/two_way.hpp         the TwoWayCoder concept; EncodingCoder, DecodingCoder
src/codec/messages.hpp        the message driver: framing, the escape, the size rules
src/codecs.cpp                registers codec 4
include/cpress/stream.hpp     ByteSource, ByteSink; compress_file, decompress_file
src/stream.cpp
src/cli/main.cpp              uses the file functions
bench/bench.cpp               messages, bytes per message, MB/s; streams codec 4
bench/columns.cpp             cpress-columns: the fair baseline's splitter
bench/baselines.sh            runs every baseline and prints the table
tests/itch_schema_test.cpp
tests/residual_test.cpp
tests/field_coder_test.cpp
tests/order_table_test.cpp
tests/message_model_test.cpp
tests/stream_test.cpp
tests/roundtrip.cpp           checksums and thresholds for itch; a synthetic ITCH sample
corpus/README.md              the day file: where from, its size and checksum
```

Hot-path code stays header-only. `include/cpress/stream.hpp` is the one
addition to the public API.

## The data

**The file.** `12302019.NASDAQ_ITCH50.gz` from
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>, the smallest ITCH 5.0 day
there: 30 December 2019. It is 3,524,013,057 bytes, MD5
`23514ef98821448a3f8f4c5b4095f3f5`, and unpacks to 8,251,407,909. The MD5
was computed on the download; Nasdaq's `.md5sum` file for it returns 404,
so the only outside check is gzip's own CRC, which passed. The roadmap's
"12,012-message test file" is not in that listing or its subdirectories;
the roadmap is amended.

**Framing.** The specification defines messages, not files. In the file
each message is preceded by its length as a 2-byte big-endian integer.
Every one of the 268,744,780 messages has a type the specification defines
and exactly the length it gives that type.

**What is in it.**

| Type | Message | Count | Share of bytes |
|---|---|---|---|
| `A` | Add order | 117,145,568 | 53.95% |
| `D` | Order delete | 114,360,997 | 29.11% |
| `U` | Order replace | 21,639,067 | 9.70% |
| `I` | Net order imbalance | 4,024,315 | 2.54% |
| `E` | Order executed | 5,722,824 | 2.29% |
| `X` | Order cancel | 2,787,676 | 0.84% |
| `F` | Add order with attribution | 1,485,888 | 0.76% |
| `P` | Trade | 1,218,602 | 0.68% |
| `L`, `C`, `Q`, `Y`, `H`, `R`, `J`, `S`, `K`, `V` | everything else | 359,843 | 0.14% |

Nine tenths of the file is orders being added and taken away again.
Before 09:00 the feed is quiet, 124 MB in six hours; from 09:00 to 16:00
it averages 1.15 GB an hour.

**Facts the design rests on,** each measured on this day:

- Timestamps are nanoseconds since midnight and never go backwards. 1.24%
  equal the one before; 97% are between 2^7 and 2^20 ns after it, with
  no pattern in the low bits (the last three decimal digits are
  uniform).
- Every message that names an order (`E`, `C`, `X`, `D`, `U`) names one
  that an earlier `A`, `F` or `U` created and that is still alive, in the
  same stock. Not once in 144,610,481 messages is it otherwise.
- At most 1,924,078 orders are alive at once. None are at the end.
- New order references come from four counters, told apart by the
  reference modulo 4. A stock uses one counter all day: no stock changed
  in 140,270,523 new references. Within a counter the next reference is
  the last plus 4 times a small number: plus exactly 4 in 56.7% of cases.
  The exceptions are bursts at 09:30, when orders entered earlier appear
  with their old, smaller references.
- 98.4% of executions take the oldest order at the best price.
- A match number is the previous one plus 1 in 99.7% of cases.
- Prices are in units of 1/10,000 dollar, and most are whole cents.

These are facts about one day, not about ITCH. The four counters and the
whole-cent prices are constants of the model (sections 4 and 5); a file
that does not follow them still round-trips and costs more. An exit
criterion checks a second day.

## The rule: one function, both directions

Hard rule 1 says the two directions share one predictor. For the bitwise
codecs that was enough: the driver owned the bytes and the model owned the
bits. Here the model decides what is coded: which field next, as a
difference from what, in how many bytes. If the encoder and the decoder
each had a copy of that logic, the copies would drift.

So there is one copy. The coder is handed to the model behind a call that
runs in either direction:

```cpp
// A coder that codes one bit in either direction. When encoding, `bit` is
// the bit to code and is left as it was; when decoding, it is set to the
// decoded bit. p12 is P(bit = 1), as everywhere.
template <typename C>
concept TwoWayCoder = requires(C& coder, int& bit, std::uint32_t p12) {
    { coder.code(bit, p12) } noexcept -> std::same_as<void>;
    { C::kDecoding } -> std::convertible_to<bool>;
};
```

`EncodingCoder` wraps `coder::Encoder`; `DecodingCoder` wraps
`coder::Decoder`. Every function above them is a template over the coder
and is written once: it takes a value, codes it, and returns it. When
encoding, the value passed in is the field read from the message, and it
comes back unchanged. When decoding, the value passed in is ignored, and
what comes back is the field, which the caller writes into the message.
The market state is then updated from the returned values, by the same
lines, in both directions.

`kDecoding` exists for the two places that must differ, both at the edge:
reading a field from the message or writing it there, and searching for an
order's place in a list or walking to a place already known.

## 1. Schema

`schema.hpp` holds what the specification says, and nothing about how it
is modelled:

```cpp
enum class FieldKind : std::uint8_t {
    kAlpha, kInteger, kPrice, kLocate, kTracking, kTimestamp, kReference, kMatch
};

struct Field {
    std::string_view name;
    std::uint8_t offset;      // from the type byte
    std::uint8_t length;
    FieldKind kind;
};

struct MessageLayout {
    char type;
    std::uint8_t length;      // without the prefix
    std::span<const Field> fields;
};

// The layout of a type, or nullptr if the byte is not an ITCH 5.0 type.
[[nodiscard]] constexpr const MessageLayout* layout_of(std::uint8_t type) noexcept;

inline constexpr std::size_t kLengthPrefixBytes = 2;
inline constexpr std::size_t kMaxMessageBytes = 50;
```

The 23 types and their lengths, without the prefix: `S` 12, `R` 39, `H`
25, `Y` 20, `L` 26, `V` 35, `W` 12, `K` 28, `J` 35, `h` 21, `A` 36, `F`
40, `E` 31, `C` 36, `X` 23, `D` 19, `U` 35, `P` 44, `Q` 40, `B` 19, `I`
50, `N` 20, `O` 48. Every type starts with the same 11 bytes: type (1),
stock locate (2), tracking number (2), timestamp (6). Take every offset
and length from the specification (the PDF dated 28 February 2024; the
link is in `docs/idea.md`), not from this brief and not from memory.
`static_assert` that each layout's fields cover its message exactly once,
in order, with no gap.

## 2. Benchmark and baselines

**`cpress-bench`** gains three columns: `messages`, the number of
well-formed ITCH messages in the input (empty for other files, which is
decided by parsing the whole file with `layout_of`); `bytes_per_message`,
the archive size over that; and `mb_per_s` each way, which is 1,000 over
the ns/byte it already reports. The header changes, so the old-file rule
of stage 4 applies again. For codec 4 it uses the file functions of
section 8 and checks the round trip by CRC-32 and size, without holding a
restored copy.

**The baselines** are other people's programs, run as commands. They are
not dependencies: nothing links them and the build does not need them.
`bench/baselines.sh FILE` runs, each on one core, timing both directions:

| Baseline | Command |
|---|---|
| gzip | `gzip -9` |
| zstd | `zstd -19 -T1` |
| zstd, its strongest | `zstd --ultra -22 -T1` |
| xz | `xz -9 -T1` |
| the text model | `cpress c --codec mix9` on the first 30,000,000 messages (`corpus/README.md`), which whole-file I/O can hold |
| the fair baseline | `cpress-columns`, then `zstd -19 -T1` on every column |

**The fair baseline** is what a careful engineer does without writing a
model. `cpress-columns FILE DIR` writes one file per column:

- `type.raw`: the type of every message, one byte each.
- For every field of every type, `T.field.raw`: its bytes as they are.
- For every field that is not text, two more: `T.field.delta`, the
  difference from the column's previous value, same width, big-endian,
  wrapping; and `T.field.dvar`, the same difference, zigzagged, as a
  LEB128 varint.
- The three header fields again as one column across all types, in file
  order: `all.locate`, `all.tracking`, `all.timestamp`, each in the three
  encodings.

Each file is compressed alone. The baseline's size is the sum, over
columns, of the smallest of its encodings; each header field is counted
once, as its per-type columns or as its one column, whichever is smaller.
The length prefixes are dropped, since the type implies them; the tool
refuses a file with a message that is not ITCH 5.0. This is generous to
the baseline in two ways: it picks each column's encoding after seeing the
result, and it is not charged for saying which it picked.

It is not charged for decoding either: no tool puts the columns back
together. The baseline is a size, with compression speed; say so in the
table.

`cpress-columns --count FILE` writes nothing and prints each type's count
and bytes: the table of [The data](#the-data), for any file.

**What it costs to run.** For the day file the columns are about 24 GB,
three encodings of everything, beside the 8.25 GB file itself; the script
must compress each column as soon as it is written and delete it, and
say at the start how much disk it needs. On the development machine the
whole script is about nine hours of one core, most of it `zstd --ultra
-22` and the columns. Say both in the script's usage text and in
`corpus/README.md`.

## 3. Field coder

A message becomes a sequence of **symbols**, each a nibble or a byte. A
symbol has an **id**, which says what it is (the top byte of a timestamp
gap; the side of an add), and four **contexts**, 64-bit numbers chosen by
the message model. The field coder turns them into six context inputs,
predicts each bit of the symbol from all six, and codes it.

```
inputs      6: HistoryTable + state map each, as mix9's
keys        key[0]     = mix(id, 0)                          the symbol alone
            key[1]     = mix(id, 1, prefix)                  and what came before it in its field
            key[2 + j] = mix(id, 2 + j, context[j], prefix)  for j = 0..3
table key   (key << kMarkerBits) | marker
marker      0 for a nibble, or a byte's first; kNibbleDone + the first nibble for a byte's second
mixer       Mixer<6>; weight set 2 * id, or 2 * id + 1 for a byte's second nibble
APM         one; context id * 256 + partial: the symbol's bits so far after a leading 1
p12         (p_mix + p_apm + 1) / 2                          in [1, 4095], so no clamp
```

`mix` is a hash of two words, chained for three and four:

```
mix(a, b)       = h ^ (h >> 29), where
                  g = (a + kHashMultiplier) * kWordKey2;  g ^= g >> 32;  h = (g ^ b) * kWordKey1
mix(a, b, c)    = mix(mix(a, b), c)
mix(a, b, c, d) = mix(mix(a, b, c), d)
```

The three multipliers are stage 2's hash multiplier and stage 3's two
word keys, which are odd 64-bit constants and nothing more; give them
names of their own here. Any good hash would do; this is the one
measured. All of it is unsigned and wraps.

Ids are dense: the symbols are listed once, in `message_model.hpp`, and a
symbol's id is its place in the list. The mixer has two weight sets per
id and the APM 256 contexts per id; with about 430 symbols that is
6.9 MiB of APM, a fixed cost of every `itch` archive. (The prototype
numbered ids more sparsely, which changes the hashes and nothing else.)

**Numbers.** A field is rarely coded as its bytes. It is coded as a
number: the count of bytes it needs, then those bytes.

```
number(u, width):                 u < 2^(8 * width)
    length = the bytes u needs, 0 to width            a nibble; prefix 0
    for each byte, most significant first:
        a byte symbol; its id says which byte it is, counted from the least significant
        prefix: starts as length; after each byte, prefix = mix(prefix, byte)
```

Zero is one nibble, and so is "what the model expected". The model knows
a byte's weight from its id, and everything above it from the prefix.

A number has `width + 1` ids: the length, then one per byte position. A
field may say that only its first `k` bytes are modelled; the rest are
coded at `p12 = 2048`, eight bits a byte, without touching a table. Only
the timestamp does: below its top two bytes there is nothing to learn,
and modelling them too was no smaller, 16.39 bits against 16.38, and
slower.

**Differences.** `residual.hpp` turns a value and an expectation into the
number that is coded, and back. Three kinds, each a pair of `constexpr`
functions that are inverses for every input:

```
wrapping(value, expected, width)   u = (value - expected) mod 2^(8 * width)
signed(value, expected, width)     d = that, read as a two's complement integer of `width` bytes
                                   u = zigzag(d) = (d << 1) ^ (d >> 63), cut to `width` bytes
stepped(value, expected, width, step)
                                   d as for signed
                                   q = floor(d / step);  r = d - q * step, in [0, step)
                                   coded: number(zigzag(q)), then r
```

`stepped` is for values that move in steps: order references by 4, prices
by 100. `r` is a symbol of its own, the field's last id, a nibble if
`step <= 16` and a byte otherwise, with the field's contexts and a prefix
that says only whether `q` was zero. It is almost always zero and almost
free. Nothing here assumes the expectation is good: a wrong one gives a
large number, never a wrong value.

**Bytes.** A field coded as it is, for text and flags: one byte symbol per
byte, the id counting from the field's first byte, the prefix starting at
0 and updated as for numbers.

```cpp
class FieldCoder {
public:
    explicit FieldCoder(int slot_bits);     // allocates; may throw std::bad_alloc

    template <TwoWayCoder C>
    [[nodiscard]] std::uint32_t nibble(C& coder, Symbol symbol, const Contexts& contexts,
                                       std::uint64_t prefix, std::uint32_t value) noexcept;
    template <TwoWayCoder C>
    [[nodiscard]] std::uint32_t byte(C& coder, Symbol symbol, const Contexts& contexts,
                                     std::uint64_t prefix, std::uint32_t value) noexcept;
    template <TwoWayCoder C>
    [[nodiscard]] std::uint64_t number(C& coder, NumberField field, const Contexts& contexts,
                                       std::uint64_t u, int modelled_bytes) noexcept;
};
```

Each returns the value coded; when decoding, the value passed in is
ignored.

Memory: six history tables of `2^slot_bits` slots, 16 bytes each; six
state maps; the mixer and the APM above.

Six inputs and the APM are each measured: five inputs cost 0.3%, four
0.8%, and no APM 0.7%
([Measurements](#measurements-behind-the-choices)).

## 4. Market state

`Market` is what the messages so far imply. It is updated once per
message, after the message is coded, from the message's fields; both
directions run the same update.

**Per stock,** indexed by the 16-bit stock locate, all 65,536 of them:

| Member | Is | Set by |
|---|---|---|
| `symbol` | the stock's 8-byte name | `R` |
| `counter` | which reference counter its last new order used, 0 to 3 | `A`, `F`, `U` |
| `newest`, `live` | its live orders, newest first, and how many | the order table |
| `last_place` | the place the last order message found its order at | `E`, `C`, `X`, `D`, `U` |
| per side: `last_add_shares` | the shares of the last add | `A`, `F` |
| `last_side` | the side of the last add | `A`, `F` |
| `last_trade_price` | the price of the last execution or trade | `E`, `C`, `P` |
| `last_price` | the price of the last order added, replaced or deleted | `A`, `F`, `U`, `D` |
| `last_mpid` | the attribution of the last attributed add | `F` |
| the last `I` message's fields | for the next `I` | `I` |

**Across stocks:** the last two message types; the last two locates; the
last tracking number; the last timestamp, and the bit widths of the last
two gaps between timestamps; the last match number; the last attribution;
the four counters' heads; the stocks in order of last use; the place the
previous message's stock had in that order; the last message of each
type.

**Reference counters.** `kReferenceCounters = 4`. A new reference `ref`
belongs to counter `ref mod 4`. Its head is the largest reference seen on
it: `head = max(head, ref)`, so that the old references of the 09:30
bursts do not drag it back.

**Stocks in order of last use.** A list of all 65,536 locates, at first in
numeric order. A message's locate is coded as its place in the list and
then moved to the front. Finding the place is a scan from the front; the
mean place is small, and the worst is the length of the list.

**The order table** maps a reference to a live order:

```
entry     32 bytes: reference (8), newer and older in its stock's list (4 + 4, as entry
          indices), shares (4), price (4), locate (2), side and whether the entry is in use (1)
bucket    kOrderWays = 8 entries, 256 bytes
table     2^order_bits entries; bucket: the top order_bits - 3 bits of reference * kHashMultiplier
insert    the first free entry of the bucket; if none, the entry with the smallest reference
          is removed first, exactly as a delete would remove it
```

Each stock's live orders form a doubly linked list through the table,
newest first. An entry never moves, so the indices stay good.

- **Why bounded.** The decoder allocates the table from a level in the
  archive, checked against a compile-time maximum, like every other table
  (`docs/style.md`, Errors). It cannot grow.
- **Why the smallest reference goes.** References grow with time, so it
  is the oldest order in the bucket, and the one least likely to be named
  again.
- **What an eviction costs.** A later message that names the order does
  not find it, and codes the reference in full (section 5). Nothing
  breaks.
- **How often.** At the benchmark's level the table has 2^23 entries for
  at most 1,924,078 live orders. If the hash spreads them evenly, a bucket
  holds 1.8 on average and is full for about one insert in 1,400 at the
  day's peak. The prototype's table was unbounded, so its number has no
  evictions in it; expect the real one to differ by less than 0.01 bits
  per message for this reason. Count evictions and report them in the
  benchmark's output, so that a table too small for its file is visible.

```cpp
class OrderTable {
public:
    explicit OrderTable(int order_bits);                         // allocates, empty
    [[nodiscard]] Order* find(std::uint64_t reference) noexcept; // nullptr if not there
    void add(std::uint64_t reference, const Order& order, Stock& stock) noexcept;
    void remove(Order& order, Stock& stock) noexcept;
    // The order `place` steps from the stock's newest; place < stock.live.
    [[nodiscard]] Order& at(const Stock& stock, std::uint32_t place) noexcept;
    // The place of `reference` among the stock's live orders, if it is
    // within `limit` of the newest; otherwise `limit`.
    [[nodiscard]] std::uint32_t place_of(const Stock& stock, std::uint64_t reference,
                                         std::uint32_t limit) const noexcept;
};
```

A cancel or execution that takes an order's shares to zero removes the
order, as the specification says a reader of the feed must.

## 5. Message model

`MessageModel` owns the `FieldCoder` and the `Market`, and codes one
message:

```cpp
class MessageModel {
public:
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = 23;
    [[nodiscard]] static constexpr int level_for_size(std::uint64_t size) noexcept;
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int level) noexcept;

    explicit MessageModel(int level);       // allocates; may throw std::bad_alloc

    // Codes the message's type. Returns it, or 0 for the escape (section 6).
    template <TwoWayCoder C>
    [[nodiscard]] std::uint32_t code_type(C& coder, std::uint32_t type) noexcept;
    // Codes the rest of a message of that type, in `message`, which is
    // read when encoding and filled when decoding. Returns false if what
    // was decoded cannot have come from the encoder.
    template <TwoWayCoder C>
    [[nodiscard]] bool code_message(C& coder, std::span<std::uint8_t> message) noexcept;
    // Codes one byte that is not part of a message.
    template <TwoWayCoder C>
    [[nodiscard]] std::uint32_t code_escaped_byte(C& coder, std::uint32_t byte) noexcept;
};
```

Fields are not coded in the order they have on the wire. The order is
chosen so that each is coded after what predicts it: the type first, then
the stock, then (for an order message) the order, and only then the
rest.

In the tables below, `prev` and `prev2` are the types of the last two
messages; `same` is whether this message's locate equals the last
message's; `|` separates a field's four contexts, and a context of
several values is their `mix`. A context given as `-` is the constant 0.

**Every message:**

| Field | Coded as | Contexts |
|---|---|---|
| type | a byte | `prev` \| `prev, prev2` \| `prev`, whether the last message's stock has a live order \| `prev, prev2`, whether the last two messages had the same locate |
| locate | its place in the stocks' order of last use: a number, wrapping, expected 0, 2 bytes. Three fields, one for `A F`, one for `E C X D U`, one for the rest | `type` \| `type, prev` \| `type, k` \| `type, prev, k` |
| the order (`E C X D U` only) | below | |
| tracking number | wrapping, expected 0 | `type` \| `type`, the last tracking number \| `locate` \| `type, prev` |
| timestamp | wrapping, expected the last timestamp; the top two bytes modelled | `type, prev` \| `type, prev, same` \| `type, w1, same` \| `type, w1, w2, prev` |

`k` is the place the previous message's stock was found at, if under 4,
and otherwise 4 plus its bit width. `w1` and `w2` are the bit widths of
the last two timestamp gaps.

**The order a message refers to.** The reference is not coded. Its place
among the stock's live orders is, newest first:

```
limit = min(stock.live, kMaxWalk)                       kMaxWalk = 4096
place = place_of(stock, reference, limit)               when encoding
place:  a number, wrapping, expected 0, 4 bytes
        contexts  type | type, locate | type, stock.last_place | type, prev, same
if place < limit:   the order is at(stock, place)
if place == limit:  the reference follows in full: signed, expected the head of the stock's
                    counter, 8 bytes; contexts  type | locate | - | -
if place > limit:   the archive is corrupt (code_message returns false)
```

The order's side, shares and price are then known before the fields that
depend on them are coded.

**New references** (`A`, `F`, and the new reference of `U`): stepped by
`kReferenceCounters`, expected the head of the stock's counter plus 4, 8
bytes. Contexts: `type` \| `type`, the stock's counter \| `type, prev` \|
`locate`. Afterwards the reference's own counter's head and the stock's
counter are updated.

**Prices** that have something to be measured against (an execution's,
a replacement's, a trade's, an imbalance's) are stepped by
`kPriceStep = 100`, 4 bytes. A new order's price has nothing as good: it
is coded as it is, and its contexts find the handful of prices its stock
is trading at.

**Add order, `A` and `F`,** after the fields above:

| Field | Coded as | Contexts |
|---|---|---|
| reference | a new reference | |
| side | a byte | `locate` \| `locate`, the stock's last side \| `type, prev` \| `type` |
| shares | wrapping, expected 0 | `locate` \| `locate, side` \| the side's last add shares`, side` \| `side` |
| stock | wrapping, expected the stock's symbol | `type` \| - \| - \| - |
| price | wrapping, expected 0 | `locate, side` \| `side` \| `shares, side` \| `locate, prev` |
| attribution (`F`) | 4 bytes | `locate` \| the stock's last attribution \| both \| the last attribution of any stock |

**Order messages:**

| Type | Field | Coded as | Contexts |
|---|---|---|---|
| `E`, `C` | shares | signed, expected the order's remaining shares | `type` \| `type`, remaining \| `locate` \| `type, prev` |
| | match number | signed, expected the last match number plus 1, 8 bytes | `type` \| `type, prev` \| - \| - |
| `C` | printable | a byte | `type` \| `locate` \| - \| - |
| | price | stepped, expected the order's price | `locate` \| printable \| - \| - |
| `X` | shares | wrapping, expected 0 | `type` \| `type`, remaining \| `locate` \| `locate`, remaining |
| `D` | nothing | | |
| `U` | new reference | a new reference | |
| | shares | signed, expected the old order's shares | `type` \| `locate` \| old shares`, side` \| - |
| | price | stepped, expected the old order's price | `locate, side` \| `side` \| `type` \| `side, prev` |

If the order was not found, its shares and price are taken as 0 and its
side as buy (for `C`'s price, the stock's last trade price is expected).

**Trade, `P`:** reference, wrapping, expected 0 (`type` \| - \| - \| -);
side, a byte (`type` \| `locate` \| - \| -); shares, wrapping, expected 0
(`type` \| `locate` \| `type, prev` \| -); stock, as for an add; price,
stepped, expected the stock's last trade price, or its last price if it
has not traded (`locate` \| `type` \| `type, prev` \| -); match number, as
for `E`.

**Net order imbalance, `I`:** each field against the same field of the
stock's last `I` message. Paired shares and imbalance shares: signed, 8
bytes (`type` \| `locate` \| - \| -, and for imbalance the third context
is the last direction). Far, near and current reference price: stepped
(`locate` \| the last cross type \| - \| -). Direction, cross type and
price variation: a byte each (its last value \| `locate` \| both \| -;
cross type has no third). Stock: as for an add.

**Every other type** (`S R H Y L V W K J h Q B N O`): each field after
the timestamp as bytes, in the context of the same field in the last
message of that type: that field's bytes \| `locate` \| both \| -. `R`
then sets the stock's symbol. These are 0.09% of the file; a model for
each would not be seen in the result.

## 6. Anything else: the escape

The registry's tests give every codec text and random bytes, and a real
file may end in half a message. Codec 4 must round-trip all of it. It
does so without a second format: at each position in the input, the next
thing is either a message or one byte.

```
at position p, with n bytes left:
    the next thing is a message if  n >= 3,
                                    type = input[p + 2] has a layout,
                                    the 2 bytes at p, big-endian, equal that layout's length,
                                    and n >= 2 + that length
    then:      code_type(type); code_message(the message); p += 2 + length
    otherwise: code_type(0);    code_escaped_byte(input[p]); p += 1
```

Type 0 is not an ITCH type, so it marks the escape, and the type symbol
of section 5 carries it for nothing: in an ITCH file it never occurs. An
escaped byte is one byte symbol whose four contexts are the last one,
two, three and four bytes of the input. After an escape the last type is
0.

- The length prefix is never coded. A prefix that does not match its
  type is not a message, and goes through the escape a byte at a time.
- Bytes that are not ITCH are compressed by what amounts to an order-4
  mixing model with an extra symbol per byte: badly. That is the right
  amount of effort. Record `enwik8`'s size once, as a curiosity.
- After garbage, the next position that looks like a message is coded as
  one, whatever its fields hold. Nothing in section 5 trusts a field: a
  wrong expectation costs bits, an unknown order is coded in full.

## 7. Codec 4

- Codec id **4**, name `itch`, summary
  `"ITCH 5.0 messages, field by field against an order table"`.
  Experimental until the stage is tagged `v0.5`. `mix9` stays the
  default: the registry's "last entry is the default" rule becomes an
  explicit default, which must leave every other codec's behaviour as it
  was.
- **Payload:** as codecs 2 and 3. One byte, the level; then the coder's
  output; then its four flush bytes.
- **Level:** log2 of the order table's entries, `kMinLevel = 10` to
  `kMaxLevel = 23`. `level_for_size(size)` is the smallest level with
  `8 * 2^level >= size`, clamped. A message that adds an order is 38
  bytes with its prefix, so below the maximum no more than one entry in
  five is ever in use and a bucket almost never fills. Each history
  table has `2^min(level, kHistoryBits)` slots, `kHistoryBits = 20`:
  larger tables gained 0.1% for four times the memory.

| Level | Order table | History tables | Fixed | Total |
|---|---|---|---|---|
| 10 | 32,768 | 98,304 | about 12 MiB | 12.3 MiB |
| 16 | 2,097,152 | 6,291,456 | | 20.2 MiB |
| 20 | 33,554,432 | 100,663,296 | | 140 MiB |
| 23 | 268,435,456 | 100,663,296 | | 364 MiB |

  The fixed part is the APM (6.9 MiB at 430 symbols), the 65,536 stocks
  (about 80 bytes each), the list of stocks, the mixer's weights and the
  maps. `memory_bytes(level)` counts every allocation; test the first two
  columns against this table and the rest against the constants they
  follow from. The day file and its first 30,000,000 messages both get
  level 23.

- **Encode:** the loop of section 6 until the input ends.
- **Decode:** the same loop, driven by what is decoded, until
  `original_size` bytes are produced. Everything is untrusted. It throws
  `cpress::Error` unless:
  1. the level is in range, checked before anything is constructed;
  2. every decoded type is 0 or has a layout;
  3. a decoded message fits in what is left of `original_size`;
  4. `code_message` returns true;
  5. `original_size` is at most `kMaxMessageExpansion` times the payload
     size, on entry and for what is left after every message;
  6. the decoder never reads past the payload, and consumes it exactly.
- **`kMaxMessageExpansion`.** A coded bit leaves at most 4095/4096 of the
  coder's interval, so a payload byte stands for at most about 22,700
  coded bits. The bitwise codecs spend 8 bits on every byte, which gives
  their ceiling of 4096. This codec can spend fewer: the cheapest
  message, an `E` whose every field is as expected, is 33 bytes from 32
  coded bits. Compute the largest bytes-per-coded-bit over the layouts
  in a `constexpr` function, `static_assert` the ceiling that follows
  from it, and say in a comment which message sets it.
- **Work per message** is bounded whatever the archive holds: a scan of
  at most 65,536 stocks and a walk of at most `kMaxWalk` orders.
  State the bound where each loop is.
- `std::bad_alloc` from constructing the model becomes `cpress::Error`,
  as for codecs 2 and 3. Share `make_levelled_model`.

## 8. Streaming

The day file is 8.25 GB, and the largest in the listing is 17.9 GB
before it is unpacked. `cpress c` reads its whole input, and
`cpress-bench` holds the input, the archive and the restored copy at
once. Neither can run codec 4 on its own benchmark.

```cpp
// include/cpress/stream.hpp
class ByteSource {                       // bytes in order, a block at a time
public:
    virtual ~ByteSource() = default;
    // Fills as much of `buffer` as it can; returns how much. 0 means the end.
    virtual std::size_t read(std::span<std::uint8_t> buffer) = 0;
};
class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual void write(ByteView bytes) = 0;
};

void compress_file(const std::filesystem::path& input, const std::filesystem::path& output,
                   const Codec& codec);
void decompress_file(const std::filesystem::path& archive,
                     const std::filesystem::path& output);
```

`Codec` gains two members, null for a codec that cannot stream:
`encode_stream(ByteSource&, std::uint64_t size, ByteSink&)` and
`decode_stream(ByteView payload, std::uint64_t original_size, ByteSink&)`.
The file functions use them when they are there and the whole-buffer
functions when they are not, so every codec works through one entry
point and codecs 1 to 3 are untouched.

- **The archive is the same bytes either way.** No format change. The
  header's CRC-32 is not known until the input has been read, so
  `compress_file` writes the header with a zero CRC, streams the payload,
  then seeks back and writes the CRC. `crc32` gains an incremental form.
  A test compares a streamed archive with `compress()`'s, byte for byte.
- **The virtual calls are per block,** 1 MiB or so, never per bit or per
  message. The driver keeps a window of the input that always holds the
  whole of the next message, and drains the coder's output vector to the
  sink when it passes a block.
- **Decompression holds the archive,** and streams only the output. The
  coder's decoder reads from one span, and refilling it mid-bit would put
  a call in the per-bit loop. The archive is an eighth of the file, 1 GB
  for the benchmark day, so this is a sound trade; streaming it too is
  left for when a file needs it.
- **A corrupt archive leaves no output.** `decompress_file` writes to a
  temporary name beside the output and renames it only after the size
  and the CRC-32 check; on any error the temporary file is removed.
- **Memory** is the model, the archive when decompressing, and a few
  blocks. Record peak memory for the day in the README.

**The trade-off.** The simplest change would have been to leave I/O alone
and benchmark on slices. Then the stage's number would be about a slice,
and a slice is not a day: its first messages refer to orders it never
saw. The benchmark is the whole day, so the codec must take it.

## 9. Tests

There is no ITCH file in the repository, and the day file is too large
for CI. Tests run on messages built in the test: a small generator makes
a well-formed stream from a seed, with adds, replaces, cancels,
executions and deletes that refer to live orders, a few stocks, and
timestamps that only grow.

**Schema**

1. Every layout's fields tile its message; every length is the
   specification's (a table in the test, typed from the PDF
   independently of the header).
2. `layout_of` returns null for every byte that is not one of the 23
   types.

**Differences**

3. Each kind and its inverse return the value, for every width, at the
   edges: 0, 1, the largest value, expectations above and below, a
   difference of exactly half the range.
4. `stepped` with steps 4 and 100: a negative difference has a remainder
   in `[0, step)`; a difference of -1 is quotient -1 and remainder
   `step - 1`.

**Field coder**

5. A nibble, a byte and a number of each width come back from a decoder
   given what an encoder wrote, for seeded random values and contexts.
6. Every prediction is in `[kProbMin, kProbMax]` over 1,000,000 symbols.
7. Two symbols with different ids and equal contexts select different
   slots; a second nibble's key differs from a first's.

**Order table**

8. An order added is found; removed, it is not; its stock's list holds
   the live orders newest first after any mix of adds and removes.
9. Nine orders forced into one bucket (search for them in the test): the
   one with the smallest reference is evicted, its stock's list and
   count are as if it had been deleted, and the other eight are found.
10. `place_of` returns the limit for an order beyond it and for one that
    is not there; `at` and `place_of` agree for every live order.
11. Under ASan, at the smallest size, 100,000 random operations read
    nothing outside the table.

**Message model**

12. Round trip: the generated stream, coded and decoded, is identical,
    at the smallest level and at one where nothing is evicted.
13. The market is the same in both directions: after every message, the
    encoder's and the decoder's `Market` compare equal.
14. Removing the order table's knowledge costs bits: the same stream
    with its references replaced by random ones still round-trips, and
    is larger by a stated margin.
15. A decoded place beyond the limit makes `code_message` return false.
    Build the case by encoding with a model whose `kMaxWalk` is larger.
16. `level_for_size` at 0, 1, 8,192, 8,193, 2^26 and 2^40; `memory_bytes`
    against the table in section 7.

**Codec**

17. Round-trip and corruption tests cover `itch` through the registry:
    the six samples, none of which is ITCH, go through the escape.
18. A seventh sample for every codec: 20,000 generated messages.
19. Mixed input: generated messages with random bytes spliced between
    them, a message cut short at the end, a prefix that disagrees with
    its type, a type byte of 0.
20. Level byte: 0, `kMinLevel - 1`, `kMaxLevel + 1`, 255 and an empty
    payload are rejected.
21. Determinism: archive CRC-32s for the samples, recorded once, equal on
    MSVC and GCC.
22. Compression happens: the generated sample's archive is under a
    stated fraction of `mix9`'s for the same bytes.
23. Mutation test: unchanged in what it proves. Every `itch` decode
    allocates and zeroes at least 12 MiB, three times `mix9`'s
    smallest, so measure the suite before choosing how many mutations
    codec 4 gets. The budget is stage 3's: 30 seconds in Release, 5
    minutes with sanitizers.

**Streaming**

24. `compress_file` and `compress()` make the same archive; so do
    `decompress_file` and `decompress()` the same output, for every
    codec.
25. Block edges: inputs of one byte less than, exactly, and one byte
    more than a block; a message that straddles two blocks.
26. A corrupt archive, a truncated one and one with a wrong CRC leave no
    output file and no temporary file.

## Measurements behind the choices

From throwaway programs, not in the repository: a parser, a column
splitter, and a prototype of the model that uses the repository's own
`HistoryTable`, `ProbabilityMap`, `Mixer`, `Apm` and coder unchanged.
The prototype codes both directions with one function, as this brief
requires, and what it wrote was decoded and compared with the input: the
whole day in the specified design, all 268,744,780 messages, among them
159,232 whose order was beyond `kMaxWalk` and coded in full. GCC 13,
`-O3`, WSL 2 on an i9-12900K.

The prototype does not have the escape (the day never needs it), the
bounded order table (section 4 says what that should cost) or dense
symbol ids. Its order table is a `std::unordered_map`, so its speed says
little about the real one's.

**The comparison,** whole day, 268,744,780 messages:

| Compressor | Bytes | Bytes/message | Ratio | Compress MB/s | Decompress MB/s |
|---|---|---|---|---|---|
| `gzip -9` | 3,475,084,753 | 12.93 | 2.37 | 2.2 | 189 |
| `zstd -19` | 2,762,730,348 | 10.28 | 2.99 | 2.2 | 627 |
| `zstd --ultra -22` | 2,744,447,899 | 10.21 | 3.01 | 1.0 | 635 |
| `xz -9` | 2,488,804,620 | 9.26 | 3.32 | 1.4 | 69 |
| `mix9` | 2,192,648,142 | 8.16 | 3.76 | 1.6 | not measured |
| The fair baseline | 2,159,565,961 | 8.04 | 3.82 | 0.8 | none |
| **The prototype** | **1,043,686,913** | **3.88** | **7.91** | **4.0** | **4.3** |

Sizes are from whole-day runs. Speeds are from one sitting with nothing
else on the machine, but not one program at a time: the prototype ran
over the whole day on one core, holding 9.4 GB of the virtual machine's
15.8 so that it could check its own output, while the others ran one
after another on a second core, compressing the first 951,200,923 bytes
and decompressing the whole day. Treat them as within ten percent. A
megabyte is 10^6 bytes.

- `mix9` was streamed through a driver of a few lines that feeds the
  model and the coder as `encode_bitwise` does; on `enwik8` it makes
  `v0.3`'s payload to the byte. Its 21 bytes of header and level are
  added.
- The fair baseline is section 2's rule exactly. Its largest columns:
  the timestamp as one column of varint differences, 644,472,183 bytes
  (19.2 bits a message, where the prototype pays 16.3); the reference of
  `D`, 337,285,232 (23.6 bits a delete, where the prototype pays 11 for
  the stock and the order together); the locate, 304,396,666; the price
  of `A`, 213,348,132; the stock name of `A`, 163,020,209; the reference
  of `A`, 153,961,350. Its speed counts compressing every encoding of
  every column to find the smallest.
- Two more generous baselines. Dropping the stock-name columns of `A`,
  `F`, `P` and `I`, which the locate implies, leaves 1,983,872,411 bytes,
  7.38 a message. Compressing the columns with `xz -9` in place of
  `zstd -19` was measured on the first 30,000,000 messages only, and is
  11.8% smaller there. The prototype is 47% under the first and, on
  those messages, 44% under the second.
- The prototype compresses faster than any of them at these settings,
  and decompresses 15 to 150 times slower than the general-purpose
  ones. Both belong in the write-up.

**The first 30,000,000 messages,** 951,200,923 bytes, the slice
`corpus/README.md` describes:

| Compressor | Bytes | Bytes/message |
|---|---|---|
| `gzip -9` | 385,060,704 | 12.84 |
| `zstd -19` | 306,219,586 | 10.21 |
| `zstd --ultra -22` | 304,533,470 | 10.15 |
| `xz -9` | 275,980,244 | 9.20 |
| `mix9` | 243,566,729 | 8.12 |
| The fair baseline | 241,223,545 | 8.04 |
| The fair baseline with `xz -9` | 212,744,908 | 7.09 |
| The prototype, decoded and identical | 118,011,212 | 3.93 |

**What each part is worth,** whole day. Each row adds one thing to the
row above:

| Design | Bytes | Bytes/message | Change |
|---|---|---|---|
| Fields in their contexts and nothing else: every value as it is, every reference in full | 1,872,246,661 | 6.967 | |
| Differences: timestamps, references and match numbers against the last; a new order's price against the last add on its side | 1,223,095,724 | 4.551 | −34.7% |
| The order table: an order by its place, its fields against the order | 1,049,421,080 | 3.905 | −14.2% |
| A new order's price as it is, in place of against the last add | 1,041,494,161 | 3.875 | −0.8% |
| **The design:** history tables of 2^20 slots in place of 2^22 | **1,043,686,913** | **3.884** | +0.2% |

Every row codes the locate by recency. And beside the fourth row, with
tables of 2^22 slots:

| Variation | Bytes | Bytes/message | Change |
|---|---|---|---|
| A new order's price against the best of eight price levels a side, with the gap between the sides as a context | 1,036,496,434 | 3.857 | −0.48% |
| That, with the locate coded as its two bytes in the context of the last two locates, in place of by recency | 1,068,775,479 | 3.977 | +3.1% on the row above |
| That first row with tables of 2^20 slots | 1,037,777,811 | 3.862 | +0.12% on it |

Differences and the order table are the stage. The order of last use is
worth three percent. The price levels are worth half a percent and are
[left for later](#left-for-later). Against the last add, a new order's
price costs 3.92 bits; as it is, in the context of its stock and side,
3.42; against the best level, 3.05.

**Where the bytes go** in the design, whole day:

| Field | Messages | Bits each | Bits per message overall | Share |
|---|---|---|---|---|
| Timestamp | 268,744,780 | 16.33 | 16.33 | 52.6% |
| Locate, order messages | 144,610,481 | 6.54 | 3.52 | 11.3% |
| Locate, adds | 118,631,456 | 6.18 | 2.73 | 8.8% |
| The order's place | 144,610,481 | 4.48 | 2.41 | 7.8% |
| Add: price | 118,631,456 | 3.48 | 1.54 | 4.9% |
| Type | 268,744,780 | 1.51 | 1.51 | 4.9% |
| Add: shares | 118,631,456 | 2.16 | 0.95 | 3.1% |
| Add: reference | 118,631,456 | 2.10 | 0.93 | 3.0% |
| Add: side | 118,631,456 | 0.86 | 0.38 | 1.2% |
| Locate, other messages | 5,502,843 | 11.10 | 0.23 | 0.7% |
| Replace: price | 21,639,067 | 2.76 | 0.22 | 0.7% |
| Replace: reference | 21,639,067 | 1.72 | 0.14 | 0.4% |
| Execution: shares | 5,822,741 | 2.53 | 0.05 | 0.2% |
| Everything else | | | 0.13 | 0.4% |

An add costs 4.06 bytes, a delete 3.65, a replace 4.14, an execution
3.52. Half of every message is its timestamp. A stock name costs a
thousandth of a bit, a match number a hundredth, an order in full 15.5
bits.

**Choices measured on a slice:** 3,000,000 messages from the
100,000,000th, about 10:35, the state built from the start of the day
and the tables empty. Sizes on a cold slice run a few percent above the
whole day's; read the differences. The rows from the price levels down
were measured on the variant that has them.

| Variation | Bytes/message | Change |
|---|---|---|
| Locate as two bytes in context; by recency | 4.078; 3.939 | −3.4% |
| Timestamp contexts from the byte length of the last gap; from the bit widths of the last two | 4.078; 4.067 | −0.3% |
| Timestamp: every byte modelled; the top two; the top one | 4.0776; 4.0763; 4.0784 | within 0.05% |
| A new order's price against the last add; against the best price of a whole book | 3.927; 3.879 | −1.2% |
| Against the best of 4, 8 and 16 levels | 3.884; 3.880; 3.880 | |
| 8 levels, and the gap between the sides as a context | 3.874 | −0.2% |
| Contexts for an add that follows a removal in its stock | 3.871 | −0.07%, not kept |
| Five inputs in place of six (the last context dropped); four | 3.884; 3.905 | +0.3%; +0.8% |
| No APM | 3.899 | +0.7% |

**Naming the order,** in bits per order message, estimated from
histograms over the whole day (a bit-width class and two more bits coded
by frequency, the rest raw), not coded:

| Way | Bits |
|---|---|
| Distance back from the head of the reference's counter | 15.1 |
| Difference from the last reference the stock's messages named | 14.7 |
| Place among the stock's live orders, newest first; the locate coded separately | 5.0, and the locate |
| Levels from the best price, and place in that level's queue from its newest; side and locate separate | 3.4 + 1.7, and the rest |

The reference itself is nearly incompressible; the order's place in a
structure is not. The two structures cost about the same, and the list
is the simpler.

## Left for later

- **Price levels.** The best eight prices at which a stock has live
  orders on each side, with a count each, kept in step with the order
  table. A new order's price against the best of them, with the gap
  between the two sides as a context, is 0.48% smaller over the day
  (the table above), and eight levels predict as well as a whole book
  (the slice). It is the first thing to add if a second day shows prices
  as they are doing worse than here.
- **Executions.** 98.4% take the oldest order at the best price, which a
  queue per price would name in a tenth of a bit. Executions are 2.2% of
  messages, so the most it could save is about half a percent, for a
  structure the design does not otherwise need.
- **Timestamps** are 52.6% of the archive and close to noise. What is
  left would have to come from knowing which messages belong to one
  event. Not explored.
- **`xz` in the fair baseline.** Measured on the slice only. If the
  write-up wants the strongest column baseline, run it on the day.
- **The encoder's look-ahead,** as in stage 4.
- **Other days, other years.** The 2026 files in the same listing are
  three to five times the size, compressed; whether four counters and
  whole cents still hold there is not known.

## Exit criteria

- [ ] On the day file, with the round trip checked by size and CRC-32:
      at most 4.00 bytes per message, and at least 45% smaller than the
      fair baseline from `bench/baselines.sh` on the same machine. The
      `cpress-bench` row in the commit message and the README.
- [ ] The comparison table in the README: every baseline of section 2
      and `itch`, in bytes, bytes per message and MB/s both ways, with
      the machine, compiler and tool versions named.
- [ ] A second day, not used while designing (any other ITCH 5.0 file
      of the listing): the same build, the same table. No target; the
      row is the result, and if the margin is much smaller there, the
      README says so.
- [ ] Rows for the ablation table, each built once and not committed:
      without the order table, without the differences, and with the
      locate coded as its bytes.
- [ ] Evictions on the day file, and peak memory in both directions.
- [ ] `enwik8` through the escape, once.
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan,
      within the time budget of test 23.
- [ ] clang-format clean, clang-tidy clean, no compiler warnings.
- [ ] README (status, results, codec table, usage, layout, roadmap),
      `cpress.hpp`, `docs/roadmap.md`, `docs/idea.md`, `corpus/README.md`
      and `CLAUDE.md` updated; tag `v0.5`.

## Suggested commits

1. The brief, and the roadmap amended with it.
2. 5a: the schema, the benchmark's columns, `cpress-columns`,
   `baselines.sh`, and the baseline rows in the README.
3. 5b: the field coder, the market state, the message model, codec 4
   and their tests, with a row for the first 30,000,000 messages.
4. 5c: streaming, and the whole-day rows.

## What review will look at

- Is there one copy of the model? Could the encoder and the decoder
  disagree about which field comes next, what it is expected to be, or
  how the market changes, and would test 13 notice?
- Does every field's expectation use only what the decoder already has
  when it reaches that field?
- Does anything trust a field: a locate, a reference, a place, a length?
  Can a crafted archive walk off a list, index outside a table, or make
  one message cost more than the stated bound?
- Is the order table's eviction exactly a delete, so that the lists and
  counts stay true?
- Are the facts about one day (four counters, a step of 100) named
  constants with a comment saying where they were measured, and does a
  file that breaks them still round-trip?
- Is every baseline's command in the script, and is the fair baseline
  counted as section 2 says: each column's best encoding, each header
  field once?
- Is the schema typed from the specification, and does every credit
  name its source?
- Would a reader new to markets understand, from the comments alone,
  what an order is, why a delete is cheap once the order is known, and
  why the stock is coded by how recently it was seen?
