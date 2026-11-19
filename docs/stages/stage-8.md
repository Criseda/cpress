# Stage 8 brief: shortcuts, codec 6

At `v1.1`, codec 5 (`words`) makes 151,906,618 bytes of `enwik9`. On this
machine mcm 0.83 `-x11` is 4.6% smaller and faster both ways. Issue #4's
reading of its source found several differences:
- a predicted-byte flag (LZP), coded ahead of the model;
- updates skipped where the model is already sure;
- SSE only where it is needed;
- a cheaper table layout.

Stage 8 takes the first three, as codec 6. The fourth is issue #19.

The work was prototyped outside the repository before this brief, as plan
item 2 on issue #4. Section 2 is that prototype's record. Codec 6 is built
to make exactly the prototype's bytes, so its measurements stand (style
guide, "Measurement").

**Exit:** codec 6 is smaller than codec 5 on `enwik8` and `enwik9`, round
trip checked, and faster both ways on a quiet machine. Codecs 3 and 5 are
no slower than at `v1.1`.

## Scope

- **FlagModel** (`src/model/flag_model.hpp`): the flag's probability.
- **Shortcuts in ContextMix:** a third template parameter. `Mix10Flag` is
  `Mix10` with it on; `Mix9` and `Mix10` are unchanged.
- **The Flagging concept** (`predictor.hpp`), and the bitwise driver coding
  a flag before a byte. A flagging model has its own expansion ceiling.
- **Codec 6, `words2`:** codec 5's transform and payload, with `Mix10Flag`.

Out of scope:
- hashing once per byte (#19);
- Huffman-shaped decisions and the transform tweaks (#4, plan items 3 and 4).

## 1. Where the idea came from

A throwaway analyser counted binary decisions on codec 5's word streams.
A match model of the lpaq kind, with a flag at a match length of 12, was
right on:
- 12.2% of `enwik8`'s word-stream bytes;
- 25.3% of `enwik9`'s.

Skipping those bytes would cut the decisions by 22% on `enwik9`. That
counted work, not size and not time. The prototype measured both.

## 2. The prototype

It was a local branch of `main` at `cab5044`, patched behind environment
switches so that one build tried every variant.
- **Sizes** are of `e30`, the first 30 MB of `enwik8`. Codec 5 makes
  6,001,588 bytes of it.
- **Times** are `cpress-bench --cpu 2 --repeat 2`, configurations
  interleaved. The machine was in use, so a comparison means something only
  within one round.

**The flag.** While the match model runs, one decision comes first: "is the
next byte the match's guess?"
- **A hit** is the whole byte. Nothing else predicts or learns it.
- **A miss** is followed by the byte's bits as usual, with *exclusion*: once
  seven bits agree with the guess, the eighth must differ, and is not coded.
- **A gate.** Both sides know the flag's probability before the flag, so it
  is coded only above a gate. Otherwise the byte is coded as usual, and the
  flag's model still learns whether the guess was right.

**v1: a flag from the match length alone.** The probability came from a map
of length class × the last four outcomes.

| e30 | Bytes skipped | Flag right | Size | Time, against all off |
|---|---|---|---|---|
| match ≥ 12 | 11.7% | 81.7% | 6,106,701 (+1.75%) | 7–13% faster both ways |
| gate 3500 | 5.2% | 90.7% | 6,029,241 (+0.46%) | 3–11% faster compressing, 0–8% decompressing |

The full model codes these bytes for about a quarter of a bit each, and a
flag right 82–91% of the time costs more than that.

**v2: a flag that knows where the guess was right.** v1's map is mixed with
two tables of bit histories keyed by the context and the guess. Section 3
describes it.

| e30, v2 | Bytes skipped | Flag right | Size |
|---|---|---|---|
| match ≥ 12, no gate, orders 4 and 6 | 11.7% | 81.7% | 6,035,113 (+0.56%) |
| match ≥ 12, no gate, orders 3 and 5 | 11.7% | 81.7% | 6,028,348 (+0.45%) |
| any match, gate 3900, orders 3 and 5 | 13.9% | 98.7% | 6,005,400 (+0.06%) |
| **match ≥ 12, gate 3900, orders 3 and 5** | **6.8%** | **99.0%** | **5,997,295 (−0.07%)** |

The orders and table sizes, measured at gate 3900:

| Orders, any match | Size | | Table entries, match ≥ 12 | Size |
|---|---|---|---|---|
| 2 and 4 | 6,008,020 | | 2^18 | 5,999,178 |
| 2 and 5 | 6,006,332 | | 2^20 | 5,997,781 |
| 3 and 5 | 6,005,400 | | **2^22** | **5,997,295** |
| 3 and 6 | 6,005,168 | | | |

Orders 3 and 6 were 232 bytes (0.004%) smaller than 3 and 5. They were not
taken because every later measurement, `enwik8` and `enwik9` among them,
was made with 3 and 5.

**Time.** Run on every match with nothing prefetched, v2 was no faster than
all off, despite skipping 14% of bytes. Its two random reads into 4 MB
tables cost what the skipped bytes saved. Two changes made it pay:
- the reads are prefetched a byte ahead, from the hints that already exist;
- the flag is weighed only on matches of 12 or more.

**Skipped work.**

| e30 | Bits skipped | Size |
|---|---|---|
| APMs, mixer's \|st\| > 1200 | 46.9% | 6,005,799 |
| APMs, \|st\| > **1500** | 35.6% | **6,001,575** |
| APMs, \|st\| > 2000 | 16.5% | 6,000,839 |
| mixer update, error < **20** | 39.4% | **6,002,126** |
| mixer update, error < 40 | 45.6% | 6,003,399 |
| mixer update, error < 80 | 51.3% | 6,006,425 |
| mixer update, error < 160 | 56.6% | 6,014,013 |

APM 1500 with mixer 20 made 6,002,230 bytes. That was 2.5% faster
compressing and 2.7% decompressing than all off, best of five rounds.

**The combination.** v2 on matches ≥ 12, gate 3900, orders 3 and 5, 2^22
entries, with both skips (APM 1500, mixer 20):

| | Codec 5 | Prototype | Change |
|---|---|---|---|
| e30 | 6,001,588 | 5,998,210 | −0.06% |
| `enwik8` | 18,846,644 | 18,838,765 | −0.04%, 7.6% of bytes skipped |
| `enwik9` | 151,906,618 | 151,497,460 | −0.27%, round trip checked |

On e30 it ran in each of three rounds:
- 5–9% faster both ways than the same build with all off;
- 5–14% faster than codec 5.

`enwik9` was not timed. The one pinned run of codec 5 then took 570 ns a
byte against its record of 365, so the machine was too busy.

## 3. Codec 6

**Format.** As codec 5's: the level, 10 to 25; the word stream's size, 8
bytes; then the stream as the bitwise driver codes it. With shortcuts, the
driver codes a byte as:
- **its flag**, if the model offers one;
- **then its bits**, unless the flag hit, skipping any bit exclusion forces.

**FlagModel.** It predicts P(the byte is the guess) from four inputs, mixed
by a 4-input mixer whose weight set is the match's length class (28 sets):
- a map of length class × the last four outcomes;
- a bit-history state for (the last 3 bytes, the guess), from a
  direct-mapped table of one byte per entry with no check, read through a
  state map;
- the same for (the last 5 bytes, the guess);
- a bias.

Each table has `level − 3` bits of entries: 2^22, 4 MiB, at level 25.

**The shortcuts in ContextMix.**

| Constant | Value | Meaning |
|---|---|---|
| `kFlagMinLength` | 12 | the match must have agreed this many bytes for FlagModel to be asked |
| `kFlagGate` | 3900 | the flag is coded if FlagModel gives the guess this p12 or more (95%) |
| `kApmSkipStretch` | 1500 | the APMs are skipped beyond this \|st\| from the mixer (over 99.7% sure) |
| `kMixerSkipError` | 20 | the mixer does not learn from a bit it predicted within 20/4096 |

A byte's first slots are selected when the flag is not coded, or misses, so
a byte the flag codes touches no history table. The flag's two entries are
prefetched in `prefetch_after()`, for the match's next guess.

**The expansion ceiling.** The bitwise driver rejects a claimed size over
`kMaxExpansion` (4096) times the payload. That bound counts eight decisions
per byte. A flag codes a byte in one, at no less than log2(4096/4095) bits,
so a payload byte can stand for about 22,700 output bytes. A Flagging
model's ceiling is therefore `kMaxFlaggedExpansion`, eight times as high.
Without it, valid archives of very repetitive input would be rejected.

**Memory:** 4.3 MiB at level 10 and 4,236.4 MiB at 25, 8 MiB more than codec
5 at 25.

## 4. Timing

One sitting on 19 November 2026: Windows 11, MSVC 19.51, the i9-12900K,
the machine otherwise quiet. `cpress-bench --cpu 2`, every round trip
checked. The builds were stage 8 at `6f3e520`, `main` before it at
`d0fb70e`, and `v1.1` at `029c588`.

**Codec 6 against codec 5.** e30, the fastest of seven rounds each way.
The five rows ran interleaved, in a different order each round.

| e30 | Compress ns/B | Decompress ns/B |
|---|---|---|
| `mix9`, `v1.1` | 447.5 | 569.0 |
| `mix9`, stage 8 | 431.7 | 526.1 |
| `words`, `v1.1` | 366.7 | 486.1 |
| `words`, stage 8 | 357.5 | 437.5 |
| **`words2`** | **334.4** | **414.8** |

`words2` is 6.5% faster compressing than `words` and 5.2% faster
decompressing. Codecs 3 and 5 are faster than at `v1.1` because of #15 and
#17, which landed after the tag.

**The driver's per-byte loop.** The rounds above cannot separate the loop's
move into two helpers, `encode_byte()` and `decode_byte()`, from #15 and
#17. A second set of five rounds compared stage 8 with `main` at `d0fb70e`:

| e30, fastest of five | Compress ns/B | Decompress ns/B |
|---|---|---|
| `mix9`, `main` | 428.2 | 529.0 |
| `mix9`, stage 8 | 435.7 | 528.5 |
| `words`, `main` | 356.2 | 440.1 |
| `words`, stage 8 | 359.1 | 438.0 |

`words` was no slower, and neither was `mix9` decompressing. `mix9`
compressing was 1.8% slower, in every round. Three more sets of six rounds
measured `mix9` compressing against variants of stage 8. Each cell is the
fastest run, then the median:

| e30, `mix9` compress ns/B | Set 3 | Set 4 | Set 5 |
|---|---|---|---|
| `main` | 432.6, 434.1 | 419.8, 423.5 | 422.3, 427.1 |
| stage 8 at `6f3e520` | 433.6, 441.1 | 437.0, 442.3 | 423.4, 437.2 |
| the old loop, kept for models without a flag | 429.6, 435.5 | | 418.1, 428.1 |
| `encode_byte()`, its flag code behind `if constexpr` | | 427.2, 438.7 | |
| no `encode_byte()`: the byte coded in `encode_bitwise()` | | | 420.5, 428.5 |

The fastest runs drift between sets by more than the effect. The medians do
not. Stage 8's median was 1.6% to 4.4% slower than `main`'s in every set,
and the two builds without a call per byte matched `main`. The likely cause
is a call to `encode_byte()` that MSVC did not inline; the disassembly was
not read. The encoder now codes each byte in `encode_bitwise()`'s loop, the
last row, with one copy of the bit loop as before. `decode_byte()` stays a
function: `mix9` decompressing showed no cost. The change moves no byte. The
other timings in this section are of `6f3e520`, before it.

**Whole files.** `enwik8` is the fastest of three runs each way, the two
codecs alternating. `enwik9` is one run each.

| | `words` | `words2` | Change |
|---|---|---|---|
| `enwik8` compress ns/B | 355.1 | **319.5** | −10.0% |
| `enwik8` decompress ns/B | 427.7 | **406.6** | −4.9% |
| `enwik9` compress ns/B | 358.8 | **292.9** | −18.4% |
| `enwik9` decompress ns/B | 423.8 | **366.1** | −13.6% |

The gain grows with the file. Section 1's count of bytes a flag would have
right was twice as high on `enwik9` as on `enwik8`.

## Exit criteria

- [x] Codec 6 makes the prototype's bytes: e30 5,998,210 and `enwik8`
      18,838,765.
- [x] `enwik9` 151,497,460 bytes, round trip checked.
- [x] Tests:
  - FlagModel's own;
  - the flag in `Mix10Flag` (when it is offered, a hit, a miss's forced bit,
    hints that change no byte);
  - the level byte of codecs 5 and 6;
  - the flagged expansion ceiling, at its extreme and on garbage;
  - the golden archives and checksums of codecs 1 to 5, unchanged.
- [x] CI green on MSVC, GCC, Clang and ASan + UBSan. *Green on `15ef5e1`.*
- [x] clang-format and clang-tidy clean, no compiler warnings. *Tidy (LLVM
      22.1) on Windows; GCC 13.3 with `-Werror` under WSL.*
- [x] On a quiet machine, on e30, best of several interleaved rounds:
  - codec 6 faster than codec 5 both ways;
  - codecs 3 and 5 no slower than at `v1.1`, since the driver's per-byte
    loop changed.

  *19 November 2026; section 4.*
- [x] `enwik9` timed for codec 6, and the write-up, the README and the
      roadmap updated. *Section 4, and the write-up's sections 6 and 7 and
      appendix.*
- [x] Tag `v1.2`. *`v1.2` tags `fe4db49`, the merge of pull request #20;
      codec 6 is frozen there.*

## What review will look at

- `Mix9` and `Mix10` take no shortcut: everything new is behind
  `kShortcuts`, and their golden archives pass.
- The two directions make the same calls in the same order: the body of
  `encode_bitwise()`'s loop and `decode_byte()` side by side.
- Exclusion never skips a bit the encoder could not have forced (an assert
  in `encode_bitwise()`).
- A hit leaves the model where `push_byte()` of that byte would: no slots
  selected, nothing learned.
