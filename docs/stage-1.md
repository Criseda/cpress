# Stage 1 brief: arithmetic coder and order-0 model

The first real codec. It is deliberately the simplest one that compresses:
the point of the stage is to get the coder, the predictor interface and the
shared encode/decode driver exactly right, because every later stage plugs
into them unchanged.

**Exit number:** `enwik8` at 64 MB or better. Expect 61 to 64 MB. (Mahoney's
order-0 coder `fpaq0` is listed on the Large Text Compression Benchmark at
about 63.4 MB; confirm the figure on that page when recording results.)

## Scope

In scope:

1. A binary arithmetic coder.
2. The `Predictor` concept.
3. A generic bitwise codec that drives any `Predictor`.
4. An order-0 model.
5. Codec id 1, `order0`, registered and made the default.
6. Tests, and results recorded in the README.

Out of scope: contexts longer than the current byte, mixing, hashing, state
machines, SIMD, streaming I/O, any speed tuning. Record the speed; do not
optimise it.

## Files

```
src/coder/arithmetic_coder.hpp   Encoder and Decoder (header-only)
src/model/predictor.hpp          Predictor concept and probability constants
src/model/order0.hpp             Order0
src/codec/bitwise.hpp            encode_bitwise<P> and decode_bitwise<P>
src/codecs.cpp                   registers codec 1
tests/check.hpp                  CHECK and the counters, moved out of roundtrip.cpp
tests/main.cpp                   runs every test group
tests/coder_test.cpp
tests/order0_test.cpp
tests/roundtrip.cpp              unchanged in behaviour
```

Hot-path code is header-only so it inlines into the driver. `src/` becomes a
private include directory for `cpress_core` and for the tests. Nothing here
is added to `include/cpress/`; the public API does not change.

## Probability convention

One convention, used everywhere and never restated differently:

- A prediction is **P(next bit = 1)**, as an unsigned integer in **12 bits**.
- Valid range is **1 to 4095** inclusive. 0 and 4096 are never produced, so
  neither symbol ever has zero probability.
- Variables holding one are named with a `p12` suffix or prefix.
- Bytes are coded **most significant bit first**.

`predictor.hpp` defines `kProbBits = 12`, `kProbMax = 4095` and `kProbMin = 1`.

## 1. Arithmetic coder

A 32-bit binary coder with no carry propagation. State is an inclusive
interval `[low, high]`, starting at `[0, 0xFFFFFFFF]`.

**Split.** For a prediction `p12`:

```
mid = low + uint32((uint64(high - low) * p12) >> 12)
```

Bit 1 takes `[low, mid]`; bit 0 takes `[mid + 1, high]`. The 64-bit product
is exact and cannot overflow.

**Encode a bit.**

```
if bit: high = mid
else:   low  = mid + 1
while ((low ^ high) & 0xFF000000) == 0:
    emit(high >> 24)
    low  = low << 8
    high = (high << 8) | 0xFF
```

**Finish.** Emit all four bytes of `low`, most significant first.

**Decode.** Start by reading four bytes into `code`, most significant first.

```
bit = (code <= mid)
if bit: high = mid
else:   low  = mid + 1
while ((low ^ high) & 0xFF000000) == 0:
    low  = low << 8
    high = (high << 8) | 0xFF
    code = (code << 8) | next_byte()
```

**Invariants** (assert them in debug builds):

- `low <= high` always, and `low < high` whenever a bit is about to be coded.
- In the decoder, `low <= code <= high`.
- `1 <= p12 <= 4095`.

**Byte accounting.** If the encoder emits *N* bytes while coding, the payload
is *N* + 4 bytes and a correct decoder reads exactly *N* + 4. This is what
makes truncated and extended payloads detectable.

**Reading past the end.** `next_byte()` returns 0 and sets a sticky `overrun`
flag once the payload is exhausted. It does not throw: the per-bit path never
throws.

**Known and accepted:** when `low` and `high` straddle a top-byte boundary
with a tiny gap, the split loses precision and a few bits are wasted. This is
inherent to the carryless design, rare, and still decodes correctly.

**Interface.**

```cpp
class Encoder {
public:
    explicit Encoder(Bytes& output) noexcept;
    void encode(int bit, std::uint32_t p12) noexcept;   // amortised; may grow output
    void finish() noexcept;                             // call exactly once
};

class Decoder {
public:
    explicit Decoder(ByteView payload) noexcept;
    [[nodiscard]] int decode(std::uint32_t p12) noexcept;
    [[nodiscard]] bool overrun() const noexcept;        // read past the payload
    [[nodiscard]] std::size_t consumed() const noexcept; // payload bytes read
};
```

`Encoder::encode` appends to a `std::vector`, which can allocate. That is the
one tolerated exception to "the per-bit loop does not allocate" in this
stage; the driver reserves ahead so it is rare. Say so in a comment.

## 2. Predictor concept

```cpp
template <typename P>
concept Predictor = std::default_initializable<P> && requires(P& p, int bit) {
    { p.predict() } noexcept -> std::same_as<std::uint32_t>;  // p12, in [1, 4095]
    { p.update(bit) } noexcept -> std::same_as<void>;
};
```

The contract, which the concept cannot express and the comment must:

- `predict()` then `update(bit)` are called strictly alternately, once per bit.
- `predict()` depends only on the bits passed to earlier `update` calls.
  This is what lets the decoder reproduce the encoder.
- A default-constructed predictor is in its initial state. Two fresh
  predictors fed the same bits produce the same predictions on every
  platform.

## 3. Bitwise codec

```cpp
template <Predictor P> void encode_bitwise(ByteView input, Bytes& output);
template <Predictor P> void decode_bitwise(ByteView payload, std::uint64_t original_size,
                                           Bytes& output);
```

Encode: for each byte, for bit 7 down to 0: `predict`, `encode`, `update`.
Then `finish`.

Decode: the mirror image, using the same `P`. This pair is the only place
the two directions exist; model code is written once.

**Untrusted input.** `decode_bitwise` must hold all of these:

1. **Bounded expansion.** Reject `original_size > payload.size() * 4096`
   before decoding. Each coded bit shrinks the interval by at least a factor
   of 4095/4096, so one payload byte can yield at most about 2,840 output
   bytes; 4096 is a safe ceiling. This bounds both work and memory by the
   size of the input.
2. **No overrun.** If the decoder's `overrun` flag is set, the archive is
   corrupt. Check it once per byte, outside the bit loop, and stop early.
3. **Exact consumption.** After the last byte, `consumed()` must equal
   `payload.size()`. Otherwise the archive is corrupt. Without this check an
   appended byte would go unnoticed.
4. **No trust in `original_size` for allocation.** Reserve at most the
   bounded figure from rule 1, and prefer letting the vector grow.

Failures throw `cpress::Error` from the driver, never from inside the coder.
The container then checks the decoded size and CRC-32 as it already does.

An empty input produces a 4-byte payload and must round-trip.

## 4. Order-0 model

The context is the bits of the current byte seen so far: `ctx` starts at 1
and becomes `ctx * 2 + bit` after each bit, returning to 1 after eight bits.
It ranges over 1 to 255, so the model is one table of 256 entries.

Each entry is P(1) in **16 bits**, starting at 32768.

```
predict:  p12 = clamp(table[ctx] >> 4, 1, 4095)
update:   if bit: table[ctx] += (65536 - table[ctx]) >> kRate
          else:   table[ctx] -= table[ctx] >> kRate
          advance ctx
```

`kRate` is the adaptation shift. Try 4 and 5 on `enwik8`, keep the better,
and report both in the commit message. With either, an entry stays within
`[15, 65521]`, so it never overflows 16 bits; state that bound in a comment
and test it.

Mind integer promotion: `std::uint16_t` operands promote to `int`. Every
store back into the table is a visible `static_cast`.

Memory: 512 bytes. It fits in L1; speed here is limited by the coder.

## 5. Registration

- Codec id **1**, name `order0`, summary `"order-0 bitwise model"`.
- It becomes the default (`default_codec()` already returns the last entry).
- The id is experimental until the stage is tagged `v0.1`.

## 6. Tests

Split the test executable into groups run from `tests/main.cpp`.

**Coder**

1. *Uniform:* 1,000,000 random bits coded at `p12 = 2048`. Payload is within
   0.1% of 125,000 bytes, plus the 4 flush bytes.
2. *Skewed:* 1,000,000 bits drawn with P(1) = 0.1, coded at `p12 = 410`.
   Payload is within 1% of the entropy, `n * H(0.1) / 8` bytes.
3. *Extremes:* all ones at 4095; all zeros at 1; and the worst case, every
   bit predicted wrongly at 1 and at 4095. All round-trip.
4. *Varying:* 1,000,000 bits, each with a pseudo-random `p12` in 1 to 4095
   from a seeded generator shared by both directions. Round-trips, and
   `consumed()` equals the payload size.
5. *Empty:* zero bits gives a 4-byte payload.

**Order-0 model**

6. After 10,000 copies of one byte, each of its eight bits is predicted with
   confidence above 4000/4096.
7. Table entries stay within `[15, 65521]` under all-ones and all-zeros.
8. Two fresh models fed the same bits give identical predictions.

**Codec**

9. The existing round-trip and corruption tests cover `order0` automatically
   through the registry. All eleven corruption cases must be rejected.
10. *Mutation test:* take one valid archive and apply 10,000 seeded random
    mutations (bit flips, truncations, appended bytes, header edits). Each
    must either throw `cpress::Error` or return normally. No crash, no hang,
    no sanitizer report.
11. *Determinism:* for the fixed samples in `roundtrip.cpp`, assert the
    CRC-32 of the whole archive against constants recorded once. CI then
    proves MSVC, GCC and Clang produce identical bytes.
12. *Compression actually happens:* the repetitive-text sample is under half
    its original size; the random sample grows by less than 1%.

## Exit criteria

- [ ] `enwik8` at 64 MB or better, with the `cpress-bench` row in the commit
      message and a Results table in the README (size, bits per byte,
      ns per byte each way, peak memory).
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan.
- [ ] clang-format clean, clang-tidy clean, no compiler warnings.
- [ ] Determinism test passes on all three compilers in CI.
- [ ] README roadmap updated; tag `v0.1`.

## Suggested commits

1. Split the tests into groups (`check.hpp`, `main.cpp`); no behaviour change.
2. Add the arithmetic coder and its tests.
3. Add the `Predictor` concept, the order-0 model and its tests.
4. Add the bitwise codec, register `order0`, add codec tests.
5. Record results in the README.

## What review will look at

- Is there exactly one place where encode and decode are paired?
- Can any archive make the decoder read out of bounds, allocate without
  limit, or run for longer than its size justifies?
- Is every probability unambiguously 12-bit or 16-bit, by name?
- Is there any floating point, or any unsigned/signed mix-up hiding behind an
  implicit promotion?
- Are the invariants written down where they hold, and asserted?
- Would the coder still be correct if a later model returned any value in
  1 to 4095 on any bit?
