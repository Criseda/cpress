# Stage 3 brief: bit histories, a match model and secondary estimation

Stage 2 gave every context a probability. Stage 3 gives it a memory: each
context records a short summary of the bits it has seen, and a learned table
turns that summary into a probability. Around the contexts it adds a model
that finds the last time the recent text appeared and predicts that it
repeats, two contexts built from words instead of bytes, and a final stage
that corrects the mixer's output. Together these are the lpaq class of
compressor.

**Exit number:** `enwik8` at 20 MB or better. Expect about 19.69 MB: a
prototype of exactly this design measured an ideal code length of
19,687,808 bytes (see
[Measurements](#measurements-behind-the-choices)). A result outside 19.6 to
19.8 MB means the code departs from the brief; find out where before tuning
anything. The roadmap's stretch figure, 18.0 MB, is not reached by this
design; [Left for later](#left-for-later) lists what was measured towards it.

## Scope

In scope:

1. A bit-history state table, designed here and generated at compile time.
2. A state map: an adaptive probability for each state, per context table.
3. A history table: checked, four-way buckets of bit-history slots.
4. A match model.
5. Two word contexts: the current word, and the current word with the one
   before it.
6. Two adaptive probability maps (APMs) after the mixer.
7. `Mix9`, the predictor that composes them, with a memory level.
8. Codec id 3, `mix9`, registered and made the default.
9. A golden archive for codec 2, which stage 2 froze.
10. Tests, and results recorded in the README.

Out of scope, each measured or considered and left for the stage named:

- An order-7 context and a third APM (measured; see
  [Left for later](#left-for-later)). Recency bits in the states (measured
  at 0.004%; see [Measurements](#measurements-behind-the-choices)).
- More than one input per context, a second mixer layer, an APM on the match
  model, a two-pass or dictionary transform (after stage 6, if at all).
- Aligning the tables to cache lines, prefetching, SIMD (stage 4). The
  bucket layout is fixed here so that stage 4 can align it without changing
  a single prediction. Record the speed; do not optimise it.
- A command-line option for the memory level.
- Any change to codecs 1 and 2, to the container header, or to the stage 2
  components they use, beyond sharing codec 2's level handling with codec 3
  (section 8), which must leave its output unchanged. Codec 3 reuses `Mixer`, `stretch`, `squash`,
  `context_key` and `kHashMultiplier` unchanged; it does not use `Counter`
  or `ContextTable`.

## Files

```
src/model/state_table.hpp     the bit-history states and their transitions
src/model/probability_map.hpp ProbabilityMap: context -> adaptive probability
src/model/history_table.hpp   HistoryTable: one context's checked buckets of states
src/model/match_model.hpp     MatchModel
src/model/apm.hpp             Apm: refines a probability given a context
src/model/mix9.hpp            Mix9, the Predictor; word hashes; levels and sizes
src/codecs.cpp                registers codec 3
tests/state_table_test.cpp
tests/probability_map_test.cpp
tests/history_table_test.cpp
tests/match_model_test.cpp
tests/apm_test.cpp
tests/mix9_test.cpp
tests/golden_test.cpp         golden archive for codec 2
tests/roundtrip.cpp           checksums and thresholds for mix9
```

Hot-path code stays header-only. Nothing is added to `include/cpress/`.

## Fixed-point conventions

Stage 2's names are unchanged. Stage 3 adds three.

| Quantity | Name | Type | Scale and range |
|---|---|---|---|
| Bit-history state | `state` | `std::uint8_t` | an index into the state table; 0 is "nothing seen" |
| State-map probability | `p22` | bits 31..10 of a `std::uint32_t` | out of 2^22; the low 10 bits are a count |
| APM entry | `p16` | `std::uint16_t` | out of 65536; 0 to 65535 |

## 1. Bit-history states

A stage 2 counter says what the bits in a context averaged to. It cannot say
how sure it is, or whether the bits have recently changed their mind. A
bit-history state records how many zeros and how many ones a context has
seen, `(n0, n1)`, with older evidence discounted, and leaves the meaning of
that to a learned table (section 2). The same state means different things
in different contexts: `(0, 3)` is strong evidence in order 6 and weak
evidence in order 1. Learning the meaning per table is what the counters
could not do.

**Transitions.** On a bit `b`, with `o` the other bit:

```
n_b = n_b + 1
n_o = discount(n_o)                   discount(n) = n            if n <= 2
                                                  = n / 2 + 1    otherwise
n_b = min(n_b, cap(n_o))              cap(0) = kRunCap   = 40
                                      cap(n) = kMixedCap = 20    for n > 0
```

- The discount ages the history. When the bit changes, the evidence for the
  old bit is halved instead of kept, so a context that switches settles on
  its new behaviour in a few bits. Counts of 1 and 2 are kept: they are too
  few to halve.
- A run, all zeros or all ones, may count higher than a mixed history,
  because how long a deterministic run has lasted is the most useful thing
  a state can tell the map.

**Numbering.** State 0 is `(0, 0)`. The rest are numbered in order of
discovery by a breadth-first search from it, following bit 0 before bit 1.
That gives **181** states. The table is built by `constexpr` code from the
rule above: no table is typed in and none is copied. `static_assert` that
there are at most 256, so a state fits a byte, and that state 0 is
`(0, 0)`, so a zeroed slot is a fresh one.

```cpp
inline constexpr int kStateCount = 181;   // computed by the generator; assert it

[[nodiscard]] constexpr std::uint8_t next_state(std::uint8_t state, int bit) noexcept;
[[nodiscard]] constexpr std::uint32_t zeros(std::uint8_t state) noexcept;   // n0
[[nodiscard]] constexpr std::uint32_t ones(std::uint8_t state) noexcept;    // n1
```

Memory: 181 × 2 transitions and 181 × 2 counts, under 1 KiB.

The design, a pair of counts with the opposite count discounted, follows the
idea of the state machines in Mahoney's PAQ8 and lpaq1. The rule, caps and
numbering here are this project's own. Confirm the credit against the
sources before writing it into a comment.

## 2. Probability map

A table of adaptive probabilities indexed by a small context. It is used
twice: as the **state map** of each history table (context: the state) and
as the match model's map (context: match length and expected bit).

```
entry        bits 31..10  p22: P(1) out of 2^22
             bits  9..0   n: updates seen, saturating at kMapCountLimit = 1023
predict      p12 = clamp(p22 >> 10, kProbMin, kProbMax)
update       rate = kMapRate16[n] = 2^17 / (2n + 3)
             if bit: p22 += ((2^22 - p22) * rate) >> 16      64-bit products, < 2^38
             else:   p22 -= (p22 * rate) >> 16
             n = min(n + 1, kMapCountLimit)
```

This is the counter's rule from stage 2 with room to keep learning: 22 bits
of probability and a count to 1023, where the counter had 13 bits and 7.
A state map entry is shared by every context in its table that is in that
state, so it sees millions of updates and can afford a slow final rate.

**Initial values.** The match model's map starts every entry at one half.
A state map starts the entry for `(n0, n1)` at `(2 * n1 + 1) / (2 * (n0 + n1) + 2)`,
the estimate the counts themselves suggest, with count 0, so its first
update replaces most of that guess.

**The clamp is needed.** Unlike the counter, `p22` can come to rest below
1024, which would shift to a `p12` of 0. Each update keeps `(2n + 1) / (2n + 3)`
of the distance, so k zeros from a fresh entry divide `p22` by about
`2k + 1`: from one half, 1,023 zeros reach about 1,024, and from the
initial value of `(40, 0)`, about 25. Once the count is saturated the rate
is 63 / 65536, and an entry below 1,041 no longer moves. Say so beside the
clamp.

```cpp
class ProbabilityMap {
public:
    explicit ProbabilityMap(std::span<const std::uint32_t> initial_p22);   // one per context
    [[nodiscard]] std::uint32_t p12(std::size_t context) noexcept;         // remembers context
    void update(int bit) noexcept;                                         // learns at that context
};
```

Memory: 4 bytes per context; 724 bytes for a state map.

## 3. History table

One table per context input. It maps a context to a **slot**: the fifteen
states of the nibble's binary tree, numbered as stage 2 numbers them, and a
check byte that says which context owns the slot.

```
slot         16 bytes: byte 0 the check, bytes 1..15 the states of nodes 1..15
bucket       4 slots, 64 bytes: one cache line once stage 4 aligns the table
table        2^slot_bits slots, 2^(slot_bits - 2) buckets
```

**Lookup**, once per nibble, as in stage 2:

```
h        = key * kHashMultiplier                       wrapping 64-bit product
bucket   = h >> (64 - bucket_bits)
check    = (h >> (56 - bucket_bits)) & 0xFF            the 8 bits below the bucket index
slot     = the first of the bucket's 4 slots whose check byte equals check;
           otherwise the slot with the lowest priority (the first, on a tie),
           which is zeroed and given the check
priority = zeros(s) + ones(s), where s is the slot's state at node 1
```

The key is stage 2's `context_key` for an order, or a word key (section 5).

- **Why a check.** Two contexts that hash to one stage 2 slot shared its
  counters, and each saw the other's history as noise. Here a stranger is
  noticed, 255 times in 256, and gets a fresh slot instead.
- **Why replace by priority.** Node 1 sees every visit to its slot, so its
  state's total is a measure of how much the slot has learned; the slot
  that has learned least is the cheapest to lose.
- **Why four ways.** A bucket is exactly one cache line, so searching it
  costs no more memory traffic than reading one slot. Stage 4's cache-line
  buckets and checksums are therefore done here, where they change the
  predictions, and stage 4 only aligns and prefetches them.
- **Empty slots.** A zeroed slot has check 0 and every state 0. A context
  whose check is 0 adopts an empty slot as its own, which is harmless: the
  slot is in its initial state either way.

```cpp
class HistoryTable {
public:
    explicit HistoryTable(int slot_bits);                      // allocates, zeroed
    void select(std::uint64_t key) noexcept;                   // once per nibble
    [[nodiscard]] std::uint8_t state(std::uint32_t node) const noexcept;   // node in 1..15
    void update(std::uint32_t node, int bit) noexcept;         // state = next_state(state, bit)
};
```

`Mix9` holds one `ProbabilityMap` beside each table: the table says which
state a context is in, the map says what that state predicts here.

Memory: `2^slot_bits * 16` bytes. Order 1 is capped as in stage 2: no more
than twice its 17 × 256 keys, rounded up to a power of two, `2^14` slots.

*Amended in implementation:* the same rule caps order 2 at twice its
17 × 65,536 keys, rounded up, `2^22` slots. It binds only at level 23, and
saves 64 MiB there. It changes no prediction: at `2^22` slots, as at `2^23`,
no bucket is asked to hold more than four order-2 keys and no two keys in a
bucket share a check, so every key keeps a slot of its own and nothing is
evicted, whatever the input. That rests on the hash, not on the rule, so a
test checks it for every order-2 key at both sizes. `enwik8` compresses to
the same archive with the cap as without it.

## 4. Match model

Long repeats are common in real data: a template, a quoted line, a
boilerplate sentence. The context orders stop at 6 bytes. The match model
finds the most recent place where the last 7 bytes occurred, measures how
far back the agreement goes, and predicts that the byte which followed then
follows now.

**State:** a history buffer of the most recent `2^(level + 4)` bytes, as a
ring; a hash table of `2^(level - 1)` 32-bit positions; the current match
pointer and length.

**At each byte boundary**, after the byte at position `pos - 1` is known:

```
if length > 0 and the byte just coded was the predicted one:
    length = min(length + 1, kMaxLength); pointer = pointer + 1
else:
    length = 0
if pos >= kMinLength:                                   kMinLength = 7
    h = (last 7 bytes * kHashMultiplier) >> (64 - hash_bits)
    if length == 0:
        candidate = table[h]
        if candidate != 0 and pos - candidate < buffer size:
            length = the number of bytes, at most kMaxLength = 65535, that agree
                     going back from candidate - 1 and pos - 1, never reading
                     a byte older than the buffer holds
            if length < kMinLength: length = 0  else: pointer = candidate
    table[h] = pos
```

Positions are stored modulo 2^32 and compared in 32-bit unsigned arithmetic;
a stale or aliased candidate costs compression, never safety, because every
read goes through the ring's mask and every candidate is verified.

**Per bit:** if `length > 0` and no bit of this byte has disagreed yet, the
expected bit is the next bit of the byte at `pointer`, and the input is

```
st = stretch(map.p12(2 * length_class(length) + expected_bit))
length_class(n) = n                 for n < 16
                = 11 + bit_width(n) for n >= 16       at most 27 for kMaxLength
```

and the map learns the bit. Otherwise the input is 0 and nothing learns.
A bit that disagrees ends the match: no more predictions this byte, and the
length is 0 at the boundary.

**Mixer context.** The match model also says how long its match is, in four
classes: none (or ended this byte), under 16, under 32, 32 or more.

Memory at the largest level: 128 MiB of buffer and 16 MiB of hash table.
The buffer is as large as one history table and holds all of `enwik8`;
the cost of a 16 MiB buffer was measured at +0.3%.

Cost: a lookup compares at most `kMaxLength` bytes. It happens on any byte
where no match is running, so the worst case is `kMaxLength` comparisons per
byte, not per file; on real data a long verification starts a long match,
and lookups are rare while it lasts. State the per-byte bound in a comment.

The prototype let the length grow past `kMaxLength`; capping it, as here,
gives the same 19,687,808 bytes on `enwik8`.

Technique: Mahoney's lpaq1 has a match model of this shape (hash of the last
bytes, backward verification, a probability per length and expected bit).
Confirm the credit, and the details credited, against the source.

## 5. Word contexts

Text is made of words, and a byte context of fixed length straddles them
badly: after "the " order 4 sees the end of one word and a space. Two more
history tables are keyed by words instead.

```
letter     A to Z, a to z, or any byte >= 128 (so UTF-8 sequences are letters)
on a byte: if letter: word = (word + lowercase(byte) + 1) * kWordMultiplier
           else if word != 0: previous = word; word = 0
```

| Table | Key, before the marker | Means |
|---|---|---|
| word | `mix(word * kWordKey1 + 1)` | the current word so far; between words, "between words" |
| bigram | `mix((word + previous * kHashMultiplier) * kWordKey2 + 2)` | the same, and the word before it |

`mix(h) = h ^ (h >> 29)`; `key = (mix(...) << kMarkerBits) | marker`, as
`context_key` builds a byte context's key. The multipliers are odd 64-bit
constants: `kWordMultiplier = 0x100000001B3` (the 64-bit FNV prime),
`kWordKey1 = 0x2545F4914F6CDD1D`, `kWordKey2 = 0xD6E8FEB86659FD93`. Any odd
constants would do; these are the ones measured. Name each.

All arithmetic is unsigned and wraps. Each table is a `HistoryTable` of
`2^level` slots with its own state map.

## 6. Adaptive probability maps

The mixer's output is right on average but biased in particular places: it
may be too sure in the middle of one byte and not sure enough at the start
of another. An APM learns the correction. For a context, it holds a curve
from the input probability to the output probability, as 33 points spaced
evenly in the stretched domain, and interpolates between them.

```
entry j of a context   p16 at st = (j - 16) * 128, for j = 0..32
initial                squash((j - 16) * 128) * 16                         at most 65,520

refine(p12, context):
    x  = stretch(p12) + 2048                    in [22, 4074]
    j  = x >> 7; w = x & 127                    j in [0, 31]
    p16 = (t[j] * (128 - w) + t[j + 1] * w) >> 7
    remember j + (w >> 6), the nearer point
    return clamp(p16 >> 4, kProbMin, kProbMax)

update(bit):
    t[k] += ((bit ? 65535 : 0) - t[k]) >> kApmRate        kApmRate = 7, signed
```

The update is an arithmetic shift of a signed difference; state why the
result stays in `[0, 65535]`.

Two APMs, both fed the mixer's output:

| APM | Context | Contexts | Memory |
|---|---|---|---|
| 1 | the partial byte `c0` (1 to 255) | 256 | 16.5 KiB |
| 2 | `c0` and the previous byte, `c0 + 256 * c1` | 65,536 | 4.1 MiB |

```
p12 = (p_mix + apm1 + 2 * apm2) / 4          each term in [1, 4095], so no clamp
```

The mixer learns from its own output, `p_mix`, not from `p12`.

*Amended in implementation:* `refine` takes its input already stretched,
`refine(st, context)`, and `Mix9` stretches `p_mix` once for both APMs
instead of each stretching it. The arithmetic and the output are the same.

Technique: secondary estimation by interpolated buckets in the stretched
domain is the APM of PAQ7 and lpaq1; *Data Compression Explained* describes
it as SSE. Find the section and confirm the details before citing it.
*Amended in implementation:* checked against the sources, the details
belong to two programs. The 33 points, their initial values and the rate
of 7 are the APM of paq8l, which updates both points around its input;
lpaq1's APM has 24 points and updates only the nearer one, as this one
does. PAQ7's source was not checked, so `apm.hpp` does not credit it. *Data
Compression Explained* covers SSE in section 4.3.3.

## 7. Mix9 and the memory level

`Mix9` owns eight history tables with their state maps (orders 1 to 6, word,
bigram), the match model, one `Mixer<9>`, the two APMs, the byte history,
the word hashes and the position within the byte.

```
predict:  st[i] = stretch(map[i].p12(table[i].state(node)))   for the 8 tables
          st[8] = the match model's input
          p_mix = mixer.mix(st)
          return (p_mix + apm1.refine(p_mix, c0) + 2 * apm2.refine(p_mix, c0 + 256 * c1)) / 4
update:   mixer.update(bit); apm1.update(bit); apm2.update(bit)
          map[i].update(bit); table[i].update(node, bit)       for each table
          the match model learns the bit
          node and c0 advance
          after 4 bits:  select every table's slot for the second nibble
          after 8 bits:  push the byte (history, word hashes, match model);
                         mixer.select(4 * byte + match length class);
                         select every table's slot for the first nibble
```

**Mixer.** Stage 2's `Mixer`, unchanged, with nine inputs and 1,024 weight
sets: the previous byte times the four match length classes. When a long
match is running the mixer should trust it and little else; when none is,
it should not. One weight set per previous byte, as in stage 2, costs
+1.1%.

**Level.** As stage 2, but the level is log2 of the slots in each history
table, and a slot is now 16 bytes. `kMinLevel = 10`, `kMaxLevel = 23`;
`level_for_size(size)` is the smallest level with `2^level >= size`,
clamped. `enwik8` gets level 23.

| Level | History tables | Match model | APMs, mixer, maps | Total (bytes) | Total |
|---|---|---|---|---|---|
| 10 | 131,072 | 18,432 | 4,385,152 | 4,534,656 | 4.3 MiB |
| 16 | 7,602,176 | 1,179,648 | 4,385,152 | 13,166,976 | 12.6 MiB |
| 20 | 117,702,656 | 18,874,368 | 4,385,152 | 140,962,176 | 134.4 MiB |
| 22 | 470,024,192 | 75,497,472 | 4,385,152 | 549,906,816 | 524.4 MiB |
| 23 | 872,677,376 | 150,994,944 | 4,385,152 | 1,028,057,472 | 980.4 MiB |

*Amended in implementation:* the level 23 row includes order 2's cap
(section 3). Without it the history tables were 939,786,240 bytes and the
total 1,095,166,336, 1,044.4 MiB.

`memory_bytes(level)` counts every allocation the model makes: the eight
history tables (order 1 capped at 2^14 slots, order 2 at 2^22), the match model's buffer and
hash table, the two APMs (16,896 and 4,325,376 bytes), the mixer's weights
(1,024 × 9 × 4 bytes), the eight state maps (181 × 4 bytes each) and the
match model's map (56 × 4 bytes). Test it against this table.
At level 10 the second APM is most of the memory; it is a fixed cost of
every `mix9` archive, small or large.

```cpp
class Mix9 {
public:
    static constexpr int kMinLevel = 10;
    static constexpr int kMaxLevel = 23;
    [[nodiscard]] static constexpr int level_for_size(std::uint64_t size) noexcept;
    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int level) noexcept;

    explicit Mix9(int level);          // allocates; may throw std::bad_alloc
    [[nodiscard]] std::uint32_t predict() noexcept;
    void update(int bit) noexcept;
};
```

## 8. Codec 3

- Codec id **3**, name `mix9`, summary
  `"bit histories, match model, word contexts, APM"`. It becomes the
  default. Experimental until the stage is tagged `v0.3`.
- **Payload:** as codec 2. One byte, the level, 10 to 23; then the coder's
  output; then its four flush bytes.
- **Decode:** codec 2's rules. An empty payload is corrupt; a level outside
  the range is corrupt, checked before anything is constructed; any level in
  range is accepted, whatever the original size.
- `std::bad_alloc` from constructing the model becomes `cpress::Error`, on
  both paths, naming the level and the memory.

The codec 2 functions in `codecs.cpp` are written for one model. Codec 3
needs the same three: construct with a memory error, encode, decode with
the level checked. Share them through a template over the model, so that
the level rules exist once; codec 2's behaviour and output must not change,
which its golden archive and checksums prove.

## 9. Freezing codec 2

Stage 2 is tagged `v0.2`, so `mix6` and its output are permanent.

- `tests/golden_test.cpp`: a small `mix6` archive as a `constexpr` byte
  array, with its text and the command that produced it, checked in both
  directions as codec 1's is.
- The existing `mix6` checksums in `roundtrip.cpp` must not change.

## 10. Tests

**State table**

1. State 0 is `(0, 0)`; there are `kStateCount` states, at most 256.
2. Every transition stays inside the table, and follows the rule: from every
   state and for each bit, the next state's counts are those the rule gives.
3. A run of 100 ones from state 0 reaches `(0, kRunCap)` and stays; a zero
   then gives `(1, discount(kRunCap))`, which is `(1, 21)`: the cap applies
   only to the count just incremented. More zeros give `(2, 11)`, `(3, 6)`,
   `(4, 4)`, `(5, 3)`, `(6, 2)`, and the run comes to rest at
   `(kMixedCap, 2)`: two ones are never discounted away. Derive every
   expected state in the test from the rule and the named constants.

**Probability map**

4. A map driven with each constant sequence stays within
   `[kProbMin, kProbMax]` for 100,000 updates, from every initial value a
   state map uses.
5. A 90%-ones random sequence settles within a stated tolerance of 0.9.
6. Updating one context leaves the others untouched.

**History table**

7. The same key finds the same slot, with its states intact, after other
   keys have been looked up.
8. Five keys forced into one bucket (search for them in the test): the four
   with the most-trained node 1 survive, the least-trained is replaced and
   its slot comes back zeroed with the new check.
9. Slot and bucket indices are in range at the smallest and largest sizes,
   for keys including 0 and all ones.

**Match model**

10. Text repeated twice: from `kMinLength` bytes into the second copy to its
    end, the model predicts every bit, the expected bit is right, and its
    length class reaches 3.
11. A mismatch ends the match for the rest of the byte; the next byte
    starts a lookup.
12. At the smallest level, an input longer than the buffer: no read outside
    the buffer (ASan), and matches older than the buffer are not used.

**APM**

13. A fresh APM is non-decreasing in its input, and differs from it by no
    more than the error of interpolating `squash` linearly between points
    128 apart, which the test computes from the table.
14. Driven with a bias (input 0.5, bits 90% ones), its output moves towards
    0.9; other contexts are untouched.
15. Entries stay in `[0, 65535]` for constant sequences of 1,000,000 bits.
    *Amended in implementation:* inputs sweep every `p12`, but the end
    points are nearest to only a few of them and do not come to rest in a
    million bits. The test checks that no point moved against the bit,
    which arithmetic that wrapped would do, and that the centre point comes
    to rest: at `65535 - (2^kApmRate - 1)` or above for ones, at 0 for
    zeros.

**Mix9**

16. 1,000,000 seeded random bits at `kMinLevel`: every prediction is in
    `[kProbMin, kProbMax]`.
17. Two fresh models at the same level give identical predictions.
18. `level_for_size` at 0, 1, 1024, 1025, 2^23 and 2^40; `memory_bytes`
    against the table above. *Amended in implementation:* also each
    table's slot count, and the order-2 check of section 3: every key's
    bucket and check, at `2^22` and `2^23` slots.

**Codec**

19. Round-trip and corruption tests cover `mix9` through the registry.
20. Level byte: 0, `kMinLevel - 1`, `kMaxLevel + 1`, 255 and an empty
    payload are rejected.
21. Any level decodes: the same data at `kMinLevel` and at `kMaxLevel`. The
    second reaches the allocation limit, about 1 GiB.
22. Determinism: archive CRC-32s for the six samples, recorded once, equal
    on MSVC and GCC.
23. Compression actually happens: thresholds for the repetitive and random
    samples, derived as stage 2 derived its own.
24. Golden archive for codec 2 (section 9).
25. Mutation test: unchanged in what it proves. Every `mix9` decode
    allocates and zeroes at least 4.3 MiB, so measure the suite before
    choosing the text length, and keep it under 30 seconds in Release and
    5 minutes with sanitizers. Put both times in the commit message.

## Measurements behind the choices

From a throwaway prototype, not in the repository. It reproduces stage 2's
22,527,145-byte ideal code length exactly before changing anything. Sizes
are ideal code lengths on `enwik8` in bytes; the archive adds 25 bytes of
header, level and flush. Memory is held equal to stage 2's unless stated:
a 16-byte history slot gives twice the slots of a 32-byte counter slot.

**Each component alone, added to stage 2:**

| Change | Bytes | Change |
|---|---|---|
| Stage 2, as shipped | 22,527,145 | |
| 16-bit check, slot reset on mismatch, one way | 22,468,637 | −0.3% |
| 16-bit check, two ways (counters) | 22,083,941 | −2.0% |
| Bit-history states, no check | 21,966,993 | −2.5% |
| States, 8-bit check, two ways | 21,572,316 | −4.2% |
| States, 8-bit check, four ways, last bit remembered while `n0 + n1 <= 6` | 21,511,297 | −4.5% |
| **The same without the last bit (this brief)** | **21,515,834** | **−4.5%** |
| Last bit remembered, and discount by `bit_width` (faster ageing) | 21,542,416 | −4.4% |
| Match model, minimum length 7 | 21,546,490 | −4.4% |
| Word context | 21,977,849 | −2.4% |
| Word and bigram contexts | 21,726,819 | −3.6% |
| APM on `c0` | 22,371,801 | −0.7% |
| APMs on `c0` and `c0, c1` | 22,206,810 | −1.4% |
| Three APMs (adds `c0` and two bytes, hashed) | 22,168,444 | −1.6% |
| Orders 1, 2, 3, 4, 6 | 22,833,981 | +1.4% |
| Orders 1 to 7 | 22,209,811 | −1.4% |

**The design, and each part taken out of it:**

| Design | Bytes | Change |
|---|---|---|
| **As specified** | **19,687,808** | |
| Counters in 32-byte slots, 16-bit check, two ways, in place of states | 20,116,285 | +2.2% |
| States, but no check: one way, as stage 2 | 20,010,413 | +1.6% |
| Neither: stage 2's tables, with the match model, words and APMs | 20,358,042 | +3.4% |
| No match model, mixer by previous byte | 20,323,201 | +3.2% |
| No word contexts | 20,364,822 | +3.4% |
| Word context, no bigram | 20,011,685 | +1.6% |
| No APMs | 19,909,285 | +1.1% |
| Mixer by previous byte alone | 19,911,256 | +1.1% |
| `kMaxLevel` 22: half the history tables, 524 MiB in all | 19,779,989 | +0.5% |

The `kMaxLevel` 22 row kept the match model at its level 23 size. At its
own level the buffer would hold 64 MiB, which was not measured; a 16 MiB
buffer cost +0.3% (below), so the true figure is between +0.5% and +0.8%.
*Amended in implementation:* built at level 22 throughout, match model
included, `mix9` makes 19,849,694 bytes of `enwik8`, +0.82%.

**Variations,** measured on the design with the last bit remembered in
small states (19,687,024 bytes, 784 fewer than the design):

| Variation | Bytes | Change |
|---|---|---|
| Match minimum length 5 / 9 | 19,840,317 / 19,705,061 | +0.8% / +0.1% |
| Match buffer of 16 MiB | 19,753,123 | +0.3% |
| State-map count limit 255 | 19,690,086 | +0.0% |
| Mixer by partial byte and match class | 19,783,631 | +0.5% |
| Orders 1, 2, 3, 4, 6 | 19,733,774 | +0.2% |

Remembering the last bit was worth 4,537 bytes alone (0.02%) and 784 in the
design (0.004%), so the states are plain counts: one idea fewer to explain,
181 states instead of 196.

Speed: run alone (GCC 13, WSL 2), the prototype took 983 ns per byte for
the design and 482 for its stage 2 baseline, about twice as long. Stage 2's
shipped code runs at 382 to 432 ns per byte, so expect `mix9` near 800 to
900 before stage 4. That is about stage 4's target of 870 already: its real
work will be the stretch figure, 500. Record the speed; it is not an exit
criterion here.

## Left for later

Measured, worth having, and not needed for the exit number. Each is a small
change with a new codec id once `mix9` is frozen, and the first candidates
if stage 6's write-up wants a smaller number.

| Addition to the design | Bytes | Change | Cost |
|---|---|---|---|
| An order-7 history table | 19,539,885 | −0.8% | 128 MiB more, a tenth input |
| A third APM, on `c0` and the last two bytes hashed to 8 bits | 19,657,643 | −0.2% | 4.1 MiB more, one more interpolation per bit |

Together they were not measured; they are unlikely to overlap much, so about
19.5 MB. What closes the rest of the gap to lpaq9m's 18.0 MB was not
measured. Read lpaq9m's description on the benchmark page before guessing;
whatever it is, it does not belong in this stage.

## Exit criteria

- [ ] `enwik8` at 20 MB or better, with the `cpress-bench` row in the commit
      message and in the README results table.
- [ ] Further rows for the ablation table, each built once and not
      committed: `kMaxLevel` 22, and the design without the match model.
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan.
- [ ] The test suite meets the time budget in test 25.
- [ ] clang-format clean, clang-tidy clean, no compiler warnings.
- [ ] README (status, results, codec table, layout, roadmap), `cpress.hpp`,
      `docs/roadmap.md` and `CLAUDE.md` updated; tag `v0.3`.

## Suggested commits

1. The brief, and the roadmap amended with it (stage 4 no longer owns
   checked buckets).
2. Stage 3: the golden archive for codec 2, the model, codec 3 and their
   tests.
3. Results in the README, citing the commit they were measured at.

## What review will look at

- Is the state table generated, not typed in, and does every constant in it
  follow from the rule and the two caps?
- Is every fixed-point value's scale clear from its name, and every product
  computed in a type wide enough for it, with the bound stated?
- Can any archive make the match model read outside its buffer, or make a
  byte cost more than `kMaxLength` comparisons?
- Can a crafted archive make the decoder allocate more than `kMaxLevel`
  allows, or throw anything but `cpress::Error`?
- Do `predict` and `update` allocate, throw, or branch on anything the
  decoder could not know?
- Is codec 2 untouched: same checksums, golden archive decoding?
- Does each component say what it predicts, what it costs in memory and
  where its technique comes from, with every credit checked?
- Would a reader new to compression understand, from the comments alone,
  why a context keeps a state instead of a probability, and what the match
  model is betting on?
