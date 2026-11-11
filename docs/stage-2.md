# Stage 2 brief: context mixing

Stage 1 predicted each bit from the current byte alone. Stage 2 predicts it
six times over, from the last one to six bytes, and learns online how much
to trust each prediction. This is the step from an entropy coder to a
compressor that beats xz and zstd.

**Exit number:** `enwik8` under 25 MB. Expect about 22.5 MB: a prototype of
exactly this design measured an ideal code length of 22,527,145 bytes (see
[Measurements](#measurements-behind-the-choices)). A result outside 22.4 to
22.7 MB means the code departs from the brief; find out where before tuning
anything.

## Scope

In scope:

1. Fixed-point `stretch` and `squash` tables, generated at compile time.
2. A 16-bit adaptive probability with a count (the *counter*).
3. A hashed context table: one per order, for orders 1 to 6.
4. A logistic mixer with 256 weight sets.
5. `Mix6`, the predictor that composes them, with a memory level.
6. Codec id 2, `mix6`, registered and made the default.
7. A golden archive for codec 1, which stage 1 froze.
8. Tests, and results recorded in the README.

Out of scope, each measured or considered and left for the stage named:

- Collision detection in the tables (stage 3). Slot entry 0 is reserved for it.
- Bit-history states, secondary estimation, a match model, word contexts
  (stage 3).
- An order-0 input, a seventh order, a second mixer or a mixer layer.
- Cache-line alignment, prefetching, SIMD (stage 4). Record the speed; do
  not optimise it.
- A command-line option for the memory level. The level is stored in the
  archive so that one can be added without a format change.
- Any change to codec 1 or to the container header.

## Files

```
src/model/logistic.hpp        stretch and squash tables
src/model/counter.hpp         the 16-bit adaptive probability
src/model/context_table.hpp   ContextTable: one order's hashed slots
src/model/mixer.hpp           Mixer<Inputs>
src/model/mix6.hpp            Mix6, the Predictor; levels and table sizes
src/model/predictor.hpp       concept no longer requires a default constructor
src/codec/bitwise.hpp         overloads that take a constructed predictor
src/codecs.cpp                registers codec 2
tests/logistic_test.cpp
tests/counter_test.cpp
tests/context_table_test.cpp
tests/mixer_test.cpp
tests/mix6_test.cpp
tests/golden_test.cpp         golden archive for codec 1
tests/roundtrip.cpp           checksums and thresholds for mix6
```

Hot-path code stays header-only. Nothing is added to `include/cpress/`.

## Fixed-point conventions

Stage 1's probability convention is unchanged. Stage 2 adds three scales.
Each has one name, used everywhere.

| Quantity | Name | Type | Scale and range |
|---|---|---|---|
| Probability of a 1 | `p12` | `std::uint32_t` | out of 4096; 1 to 4095 |
| Counter probability | `p13` | 13 bits of a `std::uint16_t` | out of 8192; 8 to 8184 |
| Stretched probability | `st` | `std::int32_t` | ln(p / (1 − p)) × 256; −2047 to 2047 |
| Mixer weight | `weight` | `std::int32_t` | 16.16 (`kWeightOne = 65536`); −8.0 to 8.0 |

Stretched values and weights are **signed**. C++20 defines `>>` on a negative
value as an arithmetic shift, so it is portable; `docs/style.md` now says so.
Signed values are never shifted left: multiply instead. Every sum that could
exceed 32 bits is computed in `std::int64_t`, and the bound that makes each
32-bit operation safe is stated beside it.

## 1. Stretch and squash

`squash(st) = 4096 / (1 + e^(−st/256))` maps a stretched value to a `p12`.
`stretch` is its inverse. Both are tables built by `constexpr` integer code:
no floating point, not even at compile time, and nothing copied from another
compressor.

**Squash**, for `0 <= x <= 2047`:

```
kDecayQ32 = e^(-1/256) in Q32, by its Taylor series:
    term = 2^32, sum = 0
    for k = 0, 1, 2, ... while term != 0:
        sum += (k even) ? term : -term
        term = term / (256 * (k + 1))          integer division
    = 4,278,222,806

decay(0) = 2^32                                 e^(-x/256) in Q32
decay(x + 1) = (decay(x) * kDecayQ32) >> 32     64-bit product, cannot overflow

d = 2^32 + decay(x)
squash(x)  = min((2^44 + d / 2) / d, 4095)      rounded to nearest
squash(-x) = 4096 - squash(x)
```

`squash` accepts any `int` and clamps it to ±`kStretchMax` (2047) first.

**Stretch**, for `1 <= p12 <= 4095`:

```
p12 >= 2048:  the least x in [0, 2047] with squash(x) >= p12
p12 <  2048:  -stretch(4096 - p12)
```

It is odd-symmetric by construction, so a 0 and a 1 are treated alike.

Known values, for `static_assert`s: `squash(0) = 2048`, `squash(256) = 2994`,
`squash(-256) = 1102`, `squash(1024) = 4022`, `squash(2047) = 4095`,
`squash(-2047) = 1`; `stretch(2048) = 0`, `stretch(3000) = 258`,
`stretch(4095) = 2026`, `stretch(1) = -2026`.

Memory: 4,095 × 2 bytes and 4,096 × 2 bytes, about 16 KiB, read every bit.

## 2. Counter

One probability, 16 bits, that adapts quickly while it has seen little and
slowly once it has seen a lot.

```
bits 15..3   p13: P(1) out of 8192
bits  2..0   n:   updates seen, saturating at kCountLimit = 7

initial      p13 = 4096, n = 0                  (0x8000)
predict      p12 = p13 >> 1
update       rate = kRate16[n] = 131072 / (2n + 3)        65536 / (n + 1.5)
             if bit: p13 += ((8192 - p13) * rate) >> 16
             else:   p13 -= (p13 * rate) >> 16
             n = min(n + 1, kCountLimit)
```

`kRate16` is `{43690, 26214, 18724, 14563, 11915, 10082, 8738, 7710}`,
computed from the formula, not typed in. The update is stage 1's rule with a
rate that depends on the count; keep the two unsigned branches, as `Order0`
has them.

**Bounds.** Both branches are monotone in `p13`, and `n` depends only on how
many updates an entry has had. So the all-ones and all-zeros sequences give
the extremes over every possible input: `p13` stays in `[8, 8184]`, and `p12`
in `[4, 4092]`, with no clamp. Prove it at compile time: a `constexpr`
function drives an entry with each sequence until it stops moving, and
`static_assert`s check the result against `kProbMin` and `kProbMax`. First
steps, for tests: ones give 6826, 7372, 7606, 7736; zeros give 1366, 820,
586, 456.

A counter never seen predicts exactly one half, which stretches to 0: a
context with no history contributes nothing to the mix. That is why no
collision detection is needed to get under the exit number.

The update rule, a probability moved by 1/(n + 1.5) of the error, is the one
in the `StateMap` of Mahoney's lpaq1 and the CM component of ZPAQ. Confirm
that against the source before writing the credit into a comment.

## 3. Context table

One table per order. It maps the last *n* bytes, plus the half of the current
byte being coded, to a **slot** of sixteen counters (32 bytes): the fifteen
nodes of the binary tree over that nibble's four bits, indexed 1 to 15 as
stage 1 indexes a whole byte, and entry 0, unused and reserved for stage 3.
A table is therefore looked up twice per byte, not eight times.

```
history    the last 8 bytes, newest in the low byte; zero before the input
context    history & (256^n - 1)
key        (context << 5) | marker
marker     0 for the first nibble; 16 + (the high nibble) for the second
slot       (key * kHashMultiplier) >> (64 - slot_bits)     wrapping 64-bit product
```

`kHashMultiplier = 0x9E3779B97F4A7C15`, 2^64 divided by the golden ratio:
Fibonacci hashing (Knuth, *The Art of Computer Programming*, vol. 3, §6.4).
A stronger hash was worth 0.08% and is not used. The key fits in 64 bits up
to order 7; `static_assert` it.

After four bits the tree node is 16 + the nibble, which is exactly the marker
for the second nibble.

```cpp
class ContextTable {
public:
    explicit ContextTable(int slot_bits);                 // allocates and fills
    void select(std::uint64_t key) noexcept;              // once per nibble
    [[nodiscard]] std::uint32_t p12(std::uint32_t node) const noexcept;  // node in 1..15
    void update(std::uint32_t node, int bit) noexcept;
};
```

A slot is half a cache line and, until stage 4 aligns the table, may straddle
two. Say so in the comment.

## 4. Mixer

Combines the six predictions in the stretched domain. Each weight set is a
vector of six weights; the set in use is chosen by the **previous byte**, so
there are 256, and it changes once per byte. Which orders deserve trust
depends on where in the text the coder is: after a space the long contexts
know little, in the middle of a word they know a lot.

```
mix:      dot  = sum of weight[i] * st[i]                  std::int64_t
          st   = clamp(dot >> 16, -kStretchMax, kStretchMax)
          p12  = squash(st)

update:   error = 4096 * bit - p12                         in [-4095, 4095]
          weight[i] = clamp(weight[i] + ((st[i] * error + 4096) >> 13),
                            -kWeightLimit, kWeightLimit)
```

- Weights start at `kWeightOne / 4`.
- `kLearningShift = 13`; the 4096 added before the shift is half of it,
  for rounding. Compute it from the shift.
- `kWeightLimit = 8 * kWeightOne`. On `enwik8` no weight exceeds 1.6, so the
  clamp never acts there. It exists because a crafted archive can feed the
  mixer any bit sequence, and signed overflow is undefined behaviour: with
  the clamp, `|st[i] * error| < 2^23` and `|weight| <= 2^19` are provable
  bounds, not observations. State them where they hold.

This is gradient descent on coding cost: the update is the derivative of the
bit's code length with respect to each weight (Mahoney, *Adaptive Weighing of
Context Models for Lossless Data Compression*, 2005).

```cpp
template <std::size_t Inputs>
class Mixer {
public:
    explicit Mixer(std::size_t weight_sets);
    void select(std::size_t weight_set) noexcept;
    [[nodiscard]] std::uint32_t mix(const std::array<std::int32_t, Inputs>& st) noexcept;
    void update(int bit) noexcept;     // learns from the last mix()
};
```

## 5. Mix6 and the memory level

`Mix6` owns six `ContextTable`s (orders 1 to 6), one `Mixer<6>`, the history
and the position within the byte.

```
predict:   st[i] = stretch(table[i].p12(node)) for each order; return mixer.mix(st)
update:    mixer.update(bit); table[i].update(node, bit) for each order
           node = node * 2 + bit
           after 4 bits:  select every table's slot for the second nibble; node = 1
           after 8 bits:  push the byte into the history; mixer.select(byte);
                          select every table's slot for the first nibble; node = 1
```

**Level.** The model is constructed with a level, `kMinLevel = 10` to
`kMaxLevel = 22`. Orders 2 to 6 each get `2^level` slots. Order 1 has only
17 × 256 possible keys (one first-nibble slot and sixteen second-nibble slots
per context), so it gets `2^min(level, 14)`: twice the keys, rounded up to a
power of two. Compute that 14 from the order; do not type it.

| Level | Slots per order (2 to 6) | Table memory |
|---|---|---|
| 10 | 1,024 | 192 KiB |
| 16 | 65,536 | 10.5 MiB |
| 20 | 1,048,576 | 160.5 MiB |
| 22 | 4,194,304 | 640.5 MiB |

`Mix6::level_for_size(size)` is the smallest level with `2^level >= size`,
clamped to the two limits: about one slot per input byte. On the first 1 MB
of `enwik8` that costs 0.9% against the maximum level, for a quarter of the
memory. `enwik8` itself gets level 22.

```cpp
class Mix6 {
public:
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = 22;
    [[nodiscard]] static constexpr int level_for_size(std::uint64_t size) noexcept;
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int level) noexcept;

    explicit Mix6(int level);          // allocates; may throw std::bad_alloc
    [[nodiscard]] std::uint32_t predict() noexcept;
    void update(int bit) noexcept;
};
```

`Mix6` has no default constructor, so two things change around it:

- **`Predictor`** drops `std::default_initializable`. The contract becomes:
  a freshly constructed predictor is in its initial state, and two
  constructed with equal arguments give the same predictions on every
  platform.
- **`bitwise.hpp`** gains `encode_bitwise(P& predictor, input, output)` and
  `decode_bitwise(P& predictor, payload, original_size, output)`, which
  require a fresh predictor. The existing forms remain, constrained to
  default-constructible predictors, and call the new ones. The encode and
  decode loops still exist exactly once each.

## 6. Codec 2

- Codec id **2**, name `mix6`, summary `"orders 1-6, logistic mixing"`.
  It becomes the default. Experimental until the stage is tagged `v0.2`.
- **Payload:** one byte, the level; then the coder's output; then its four
  flush bytes.
- **Encode:** `level = Mix6::level_for_size(input.size())`.
- **Decode** adds these to the rules of `decode_bitwise`, which then apply to
  the payload after the level byte:
  1. An empty payload is corrupt.
  2. A level outside `[kMinLevel, kMaxLevel]` is corrupt. Check before
     constructing anything.
  3. Any level in range is accepted, whatever the original size: the level
     is what the archive says it is.
- `std::bad_alloc` from constructing the model becomes `cpress::Error`
  ("not enough memory …", naming the level and the size), on both paths.

**Why the level is in the archive.** A fixed table size would be simpler, but
every decode would then allocate and fill 640 MiB, including each of the
10,000 mutation-test decodes and every one-kilobyte file. Deriving the size
from the header's original size would hide a format rule inside a heuristic
and size an allocation by an untrusted length. One explicit byte, bounded by
a compile-time maximum, does neither, and the decoder never needs to know how
the encoder chose. `docs/style.md` now names model tables as the one
allocation sized by an archive field.

## 7. Freezing codec 1

Stage 1 is merged. From this stage on, `order0` and its output are permanent.

- `tests/golden_test.cpp`: a small `order0` archive as a `constexpr` byte
  array, with the text it must decode to and the command that produced it.
  It must keep decoding forever.
- The existing `order0` checksums in `roundtrip.cpp` must not change.
- Replace "experimental until `v0.1`" for codec 1 in the README,
  `cpress.hpp` and `codecs.cpp` only if `git tag -l v0.1` shows the tag.
  If it does not, leave the wording and say so in the hand-over.

## 8. Tests

**Logistic tables**

1. Every `squash` entry is within 0.51 of the real value, computed in the
   test with `std::exp`: the table is the real function rounded to nearest.
2. `squash` is non-decreasing and `squash(-x) == 4096 - squash(x)`.
3. `stretch` is non-decreasing and odd; `0 <= squash(stretch(p)) - p < 4` for
   `p >= 2048`.
4. The known values above, as `static_assert`s.

**Counter**

5. The first four steps of the all-ones and all-zeros sequences.
6. `p13` reaches `kP13Min` and `kP13Max` and stays there; `n` stops at
   `kCountLimit`.
7. A 90%-ones random sequence settles within a stated tolerance of 0.9.

**Context table**

8. The same key selects the same slot, and a slot index is always in range:
   at the smallest and the largest slot count, for keys including zero and
   one with every bit set.
9. Updating one node leaves the slot's other fourteen untouched.
10. At the smallest size, many keys share a slot and the table still returns
    valid probabilities.

**Mixer**

11. Two inputs, one always right and one always wrong: after 1,000 bits the
    first weight is positive, the second negative, and the output agrees with
    the first input.
12. Every input at ±`kStretchMax` and the bit always the opposite, for
    1,000,000 bits: weights reach `kWeightLimit` and stay within it. This is
    the case UBSan must pass.
13. Weight sets are independent: training one leaves the others at their
    initial value.

**Mix6**

14. 1,000,000 seeded random bits at `kMinLevel`: every prediction is in
    `[kProbMin, kProbMax]`.
15. Two fresh models at the same level give identical predictions.
16. `level_for_size` at 0, 1, 1024, 1025, 2^22 and 2^40; `memory_bytes`
    against the table above.
17. A short text repeated: by the third repetition every bit is predicted
    with confidence above a threshold derived in a comment.

**Codec**

18. Round-trip and corruption tests cover `mix6` through the registry.
19. *Level byte:* levels 0, `kMinLevel - 1`, `kMaxLevel + 1` and 255 are
    rejected; so is an empty payload.
20. *Any level decodes:* the same data encoded through the driver at
    `kMinLevel` and at `kMaxLevel` both decode through codec 2. The second
    is the test that reaches the allocation limit.
21. *Determinism:* archive CRC-32s for the six samples, recorded once.
22. *Compression actually happens:* the repetitive-text sample is under 0.5%
    of its size (the prototype's payload was 96 bytes of 90,000), and the
    random sample grows by under 0.5% (prototype: 0.29%).
23. *Mutation test:* unchanged in what it proves. `mix6` costs about a
    microsecond a byte, twenty times `order0`, so the test must not decode
    4.5 KB ten thousand times per codec. Shrink its text until the whole
    suite runs in under 30 seconds in Release and under 5 minutes with
    sanitizers; keep 10,000 mutations. Put both times in the commit message.

## Measurements behind the choices

From a throwaway prototype, not in the repository. Sizes are ideal code
lengths on `enwik8` in bytes (the sum of −log2 of each `p12`), at level 22
unless stated. They are design evidence, not benchmark rows.

| Design | Bytes | Change |
|---|---|---|
| **As specified** | **22,527,145** | |
| As specified, at level 20 (160 MiB) | 23,459,865 | +4.1% |
| Weight set chosen by the partial byte, not the previous byte | 22,611,948 | +0.4% |
| The early design: that, and a 12-bit probability with a 4-bit count, limit 15 | 22,816,779 | +1.3% |

The rest were measured against the early design, one change at a time:

| Variation on the early design | Bytes | Change |
|---|---|---|
| One weight set | 23,091,701 | +1.2% |
| Count limit 4 / 7 / 10 | 22,661,169 / 22,628,805 / 22,683,812 | −0.7% / −0.8% / −0.6% |
| 32-bit entries (22-bit probability), same memory | 23,180,623 | +1.6% |
| 32-bit entries, twice the memory | 22,759,813 | −0.2% |
| 32-bit entries, twice the memory, count limit 127 | 23,544,708 | +3.2% |
| Fixed rate, stage 1's rule: shift 4 / shift 3 | 23,681,672 / 23,117,022 | +3.8% / +1.3% |
| Level 18 / 20 / 24 (40 MiB / 160 MiB / 2.1 GiB) | 25,365,963 / 23,794,173 / 22,306,383 | +11.2% / +4.3% / −2.2% |
| Orders 1–5 / orders 1–7 | 23,474,387 / 22,500,280 | +2.9% / −1.4% |
| An order-0 input added | 22,722,145 | −0.4% |
| 16-bit check in entry 0, slot reset on mismatch | 22,687,292 | −0.6% |

Also measured: the mixer's learning rate is flat within 0.3% across a factor
of seven, and a shift of 13 matches the best; initial weights of 0, 1/6, 0.3
and 0.5 differ by 0.01% on `enwik8`, but on its first 10 KB zero costs 13%
against 0.25; a weight set chosen by the previous byte *and* the bit position
(2,048 sets) gains a further 0.6% and is left out.

Speed in the prototype was about 1.2 µs per byte.

## Exit criteria

- [ ] `enwik8` under 25 MB, with the `cpress-bench` row in the commit
      message and in the README results table.
- [ ] Two further rows, each built once and not committed, as stage 1 did for
      its adaptation shift: `kMaxLevel` 20, and a single weight set. They
      start the ablation table the stage 6 write-up needs.
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan.
- [ ] The test suite meets the time budget in test 23.
- [ ] clang-format clean, clang-tidy clean, no compiler warnings.
- [ ] README (status, results, codec table, layout, roadmap), `cpress.hpp`
      and `CLAUDE.md` updated; tag `v0.2`.

## Suggested commits

1. The brief and the documents amended with it.
2. The golden archive for codec 1.
3. Stage 2: the model, the codec and their tests.
4. Results in the README, citing the commit they were measured at.

## What review will look at

- Is every fixed-point value's scale clear from its name, and every signed
  operation provably free of overflow for any bit sequence?
- Is there floating point anywhere in `src/model/` or `src/coder/`, compile
  time included?
- Can a crafted archive make the decoder allocate more than `kMaxLevel`
  allows, or throw anything but `cpress::Error`?
- Do `predict` and `update` allocate, throw or branch on anything the
  decoder could not know?
- Is the model still written once? The codec may differ between directions
  only in who supplies the level.
- Does each component say what it predicts, what it costs in memory and
  where its technique comes from?
- Would a reader new to compression understand why mixing happens in the
  stretched domain, from the comments alone?
