# Stage 10 brief: up the frontier, codec 8

At `v1.3`, codec 7 (`words3`) makes 144,613,104 bytes of `enwik9`, and
144,734,323 with its source zipped: 39th of the 238 programs in the Large
Text Compression Benchmark's main table of 17 November 2026 [1], with
cpress's Windows record for its times. Around it:

| Program | Total, bytes | Compress ns/B | Decompress ns/B | Memory, MB |
|---|---|---|---|---|
| durilca'kingsize | 127,783,295 | <u>1,341</u> | <u>1,466</u> | 13,000 |
| fxv | 129,299,169 | 20,763 | | 1,628 |
| paq8hp12any | 132,375,726 | 37,660 | 37,584 | <u>1,850</u> |
| drt\|lpaq9m (38th) | 144,054,338 | <u>868</u> | <u>898</u> | <u>1,542</u> |
| **cpress `words3` (39th)** | **144,734,323** | **<u>345</u>** | **<u>409</u>** | **about 6,950** |
| mcm 0.83 `-x11` (40th) | 144,934,149 | 394 | <u>281</u> | 5,961 |

The underlines are as the table would stand with `words3` in it. Nothing
between paq8hp12any and lpaq9m is faster than durilca or uses less
memory than fxv.

The benchmark underlines a number when no smaller program does better on
it: three frontiers, one for each of compressing time, decompressing time
and memory. `words3` is on both time frontiers, with room: every smaller
program takes at least 868 ns a byte compressing and 898 decompressing.
It is on no memory frontier: lpaq9m is smaller and needs 1,542 MB.

**The memory `words3` would be listed with** is its peak, not its model.
`cpress-bench` puts compressing `enwik9` at 6,624.7 MiB (6,946 MB): the
model's 4,756 MiB, the 1,000 MB input, the 624 MB word stream and the
archive, each held whole. Decompressing holds the same less the input
and more the output. That is more than mcm's 5,961 MB.

## Where the frontier can be moved

**Past lpaq9m, in size.** It is 680,015 bytes (0.47%) smaller than
`words3`'s total. Under it, cpress would place 38th and lpaq9m would
leave both time frontiers. The next smaller program on a time frontier is durilca, 11%
smaller still, so after lpaq9m the time frontiers hold up to 1,341 and
1,466 ns a byte: the time is there to spend on size.

**Onto the memory frontier.** Below lpaq9m's 1,542 MB if `words3`'s size
stays, or fxv's 1,628 MB if it passes lpaq9m. Measured on 27 November, codec 7 with its tables
capped at a smaller level, `cpress-bench --cpu 2 --repeat 1` on `enwik9`,
round trip checked, the machine not quiet:

| Largest level | `enwik9`, bytes | Change | Model, MiB | Peak (bench), MiB | Compress ns/B | Decompress ns/B |
|---|---|---|---|---|---|---|
| 25 (`v1.3`) | 144,613,104 | | 4,756 | 7,421.7 | 344.7 | 409.0 |
| 24 | 146,332,662 | +1.19% | 2,416 | 5,083.3 | 353.2 | 413.6 |
| 23 | 148,602,274 | +2.76% | 1,244 | 3,916.7 | 348.2 | 407.2 |

Each halving of the tables costs 1.2 to 1.6% on `enwik9`, and at level
23 the buffers, 2.7 GB, are still more than the frontier allows. With
this table layout, the memory frontier costs about 2.8% and a streaming
driver, and gives back the place past mcm: 148.6 million is behind mcm's
144.9 and only just ahead of nanozip's 149.3.

**Under mcm's decompressing time.** 281 ns a byte, 31% faster than
`words3`. That needs the decoder to stop waiting for memory (stage 9,
section 2), which is a new table layout (issue #19), unmeasured. It
would move no underline of cpress's own: `words3` is already on that
frontier, and only mcm's underline would go.

## The choices

The owner decided on 27 November: past lpaq9m in size first, the memory
frontier after it with a denser table layout, its own stage; and the
streaming driver, as its own pull request after codec 8.

**What codec 8 is for.** **Recommendation:** past lpaq9m in size, with
the time frontiers kept and lpaq9m left behind on time as well: under
144,054,338 bytes of `enwik9` with the source zipped, compressing under
868 ns a byte and decompressing under 898. That is the one move that
changes three underlines at once (cpress's place, and lpaq9m's two), and
a size is the same on any machine, where a time is each tester's own.

**Memory.** The memory frontier and lpaq9m's size pull opposite ways with
this table layout: every halving costs more than the 0.47% the size
target needs. **Recommendation:** not the memory frontier in this stage.
No more model memory than mcm's 5,961 MB, stage 9's limit (order 6 back
makes about 5,280 MiB), and a driver that streams
rather than holds the input, the word stream and the output, which
changes no byte and brings the listed memory to about the model's,
under mcm's. The frontier itself comes back with a denser table
layout (After 1.0, issue #19), where it is a question of the layout and
not of throwing away contexts.

**The streaming driver is a restructure.** The codec interface takes and
returns whole buffers, and the word transform builds its dictionary from
the whole input. Streaming means reading the input twice (once to build
the dictionary and the stream's size, once to code), and decoding into
the inverse transform as the stream comes. It changes the code of every
codec's driver but no codec's output, so it would serve codecs 5 to 8.
**Recommendation:** do it, as its own pull request after codec 8, so
that the size and the memory are measured separately.

## Scope

**10a. The prototype.** A throwaway branch of `v1.3`, patched behind
environment switches as stage 9's was, so that one build tries every
variant. Each candidate is measured for size on `e30` and, if it pays,
on `enwik8` and `enwik9`, then combined and timed on a quiet machine.

| | Candidate | Measured before |
|---|---|---|
| C1 | Order 6 back | enwik9 −0.28% with codec 7's other parts (stage 9, section 2), decode +14% |
| C2 | A second mixer, its weights chosen by something the first does not see, and the two averaged or mixed | codec 5: −0.23% on `enwik8` |
| C3 | A third APM, the partial byte and the last two bytes | codec 5: −0.18% on `enwik8`; with C2, −0.43% for +17% time |
| C4 | The word's length in the mixer's choice of weights | mcm: worth 0.35% to it (stage 9, section 1) |
| C5 | Order 0 as an input; order 8 | mcm: 0.23% and 0.17% |
| C6 | Two contexts in one slot's line for the high orders, so a table costs one fetch for two orders | stage 9's `share6` kept half of order 6's worth |
| C7 | The state maps' and APMs' rates, and the mixer's final rate, retuned for `words3`'s inputs | not measured since stage 3 |

The candidates add time. Stage 9's decoding came to 409 ns a byte and
the budget is 898, so C1 to C3 together, at about 1.3 times, fit.

**10b. The design.** Codec 8's design written into this brief from 10a's
record, before its code.

**10c. Codec 8.** Built to make exactly the prototype's bytes. `enwik9`:
the size, both speeds, peak memory and the source zip; the write-up and
README updated; tag `v1.4`.

**10d. The streaming driver,** if the owner takes it: its own pull
request, measured for peak memory, with no change to any archive.

Out of scope:

- Any change to the output of codecs 1 to 7.
- Anything fitted to `enwik9`: constants chosen on it, or a dictionary
  shipped with the program. Constants are chosen on `e30` and `enwik8`,
  as before.
- A new table layout (issue #19), threads, and hand-written SIMD.
- A neural mixer, or anything else at cmix's hundreds of times the time.
- Sending the results to the benchmark. That is the owner's.

## 1. The prototype

First runs, 27 November, on stage 9's prototype builds (`H:\cp9`, local,
never pushed), with codec 7's switches; every round trip checked by
`cpress-bench --cpu 2 --repeat 1`, the machine not quiet, so the times
are rough. With no switch added, the build makes codec 7's bytes.

| Added to codec 7 | `e30`, bytes | Change | `enwik9`, bytes | Change | Compress ns/B | Decompress ns/B | Model, MiB |
|---|---|---|---|---|---|---|---|
| nothing (codec 7) | 5,729,199 | | 144,613,104 | | 344.7 | 409.0 | 4,756 |
| a second mixer, averaged | 5,721,625 | −0.13% | | | | | |
| a second mixer, weights chosen as `words2`'s per bit, and a final mixer of the two (C2) | 5,718,776 | −0.18% | | | | | |
| a third APM, order 2 hashed to 12 bits × partial byte (C3) | 5,722,955 | −0.11% | | | | | |
| C2 and C3 | 5,711,604 | −0.31% | 144,064,328 | −0.38% | 517.6 | 607.9 | 4,822 |
| order 6 (C1) | 5,726,540 | −0.05% | | | | | |
| C1, C2 and C3 | 5,707,888 | −0.37% | 143,662,948 | −0.66% | 558.5 | 652.9 | 5,334 |

With the source zip, about 125,000 bytes, C2 and C3 alone come to about
144.19 million, short of lpaq9m's 144,054,338; with C1 as well, about
143.79 million, some 265,000 bytes under it, within both times and
mcm's memory. The times on `e30` (not shown) rose more, as stage 9 found:
there, page faults dominate.

**Round 1,** 28 November, the machine quiet, new switches in the same
prototype (patch in `H:\cpress-corpus\stage10\proto\`). Each on top of
C1, C2 and C3 (5,707,888 on `e30`):

| Variant | `e30`, bytes | Change |
|---|---|---|
| the second mixer's sets chosen by a 12-bit hash of the last two bytes, not the last byte and match length | 5,699,524 | −0.15% |
| the second mixer's sets by the last byte and the class before it | 5,703,305 | −0.08% |
| the second mixer's sets by word length class, class of the last byte and match length (C4) | 5,711,676 | +0.07% |
| a third mixer, its sets as in the row above (C4), into the final mixer | 5,703,269 | −0.08% |
| the mixers skip updates as codec 7's does, the final mixer never | 5,705,075 | −0.05% |
| APM 3 keyed by a 12-bit hash of the last three bytes, not two | 5,706,285 | −0.03% |
| APM 3 keyed by the word | 5,713,701 | +0.10% |
| APM 3 keyed by the last three classes | 5,709,898 | +0.04% |
| the final mixer's sets also by match length | 5,708,436 | +0.01% |
| the final mixer's sets also by the class of the last byte | 5,708,077 | +0.00% |

Skipping the updates is smaller and faster both: a mixer that learns
from a bit it already predicted within 20/4096 moves its weights toward
noise. The word length is worth more in a mixer of its own than in the
second mixer's choice, where it replaces the hash.

**Round 2,** the winners together, `e30` then `enwik9`, every round trip
checked:

| Configuration | `e30`, bytes | `enwik9`, bytes | Change from codec 7 | Compress ns/B | Decompress ns/B |
|---|---|---|---|---|---|
| R2: C1, the hashed second mixer, C3, the skip | 5,696,026 | 143,319,068 | −0.89% | 605.6* | 683.8* |
| R2, APM 3 by three bytes | 5,693,134 | | | | |
| R2, the third mixer | 5,689,405 | | | | |
| **R2, both** | **5,686,639** | **143,070,644** | **−1.07%** | **658.7** | **757.3** |

\* A build ran during part of this one.

R2 with both is codec 8: 0.17% smaller than R2 for 9% more time, and
with the source zip about 143.2 million, some 850,000 bytes under
lpaq9m, compressing 24% and decompressing 16% faster than its listed
times.

Not tried: C5, order 0 and order 8 (order 8's table would take the model
past mcm's memory); C6, two orders in one line; and C7, the rates. The
target was passed without them, and each is a candidate for later.

**What lpaq9m is.** The benchmark's notes say: lpaq1 (Matt Mahoney,
2007) is orders 1 to 4 and 6, a word and a match model, in nibble hash
tables of bit histories, one mixer chosen by a small context, and two
APMs; Alexander Rhatushnyak's lpaq9 series tuned it for English text and
put drt in front, a dictionary transform whose English dictionary ships
with the program and is counted in its size. cpress's model is a
superset of lpaq1's. What lpaq9m has that cpress does not is the shipped
dictionary, built from English rather than from the input, which this
project rules out. The part of the idea that carries over, codes that
group similar words, cpress's transform already does in its own way: its
dictionary is alphabetical within each code length, so words that share
a beginning share a code's first byte.

**What the second layer takes from where.** Several mixers of the same
inputs with differently chosen weights, and a mixer of their outputs, are
paq8's (Matt Mahoney, 2005 onward), where a final mixer combines the
first layer's outputs. The choices of weights, the update skip and the
APM's context here are this project's, from the rounds above.

## 2. Codec 8

**What it takes.** Codec 7's model, `Mix11`, with order 6 back and a
second layer, as R2 with both:

- **Two more mixers** of the same thirteen inputs. One chooses its weight
  set by a 12-bit hash of the last two bytes and the partial byte,
  1,048,576 sets of 64 bytes, 64 MiB. The other by the word's length
  class (no letters, 1, 2, 3 or 4, 5 or 6, 7 or more), the last byte's
  class, the match's length class and the partial byte, 98,304 sets,
  6 MiB. The word's length counts the letters since the last byte that is
  not one, the transform's codes included, as the word contexts do.
- **A final mixer** of the three mixers' outputs, a weight set per partial
  byte. Every mixer's rate decays, as codec 7's does.
- **The update rule.** The three mixers skip a bit their final output
  predicted within 20/4096, as codec 7's mixer does; the final mixer
  learns every bit coded bit by bit.
- **APM 3,** keyed by a 12-bit hash of the last three bytes and the
  partial byte, 1,048,576 contexts, 66 MiB. The output is the mean of the
  final mixer's and the three APMs', where codec 7 weighs APM 2 twice; the
  APMs are skipped as before.

The hash is the top 12 bits of the low 32 bits of the bytes times
0x9E3779B1, 2^32 over the golden ratio.

**Where it lives.** A sixth template parameter of `ContextMix`, `Layered`,
which needs the classes and the shortcuts; `Mix12 =
ContextMix<7, 25, true, true, 0, true>`. The layer is a member struct that
models without it hold as an empty one, so codecs 3, 5, 6 and 7 compile
to what they were. The next bit's weight sets and APM 3's curve are
prefetched, as codec 7's set is; neither changes a byte.

**Format.** Id 8, name `words4`. The payload is `words3`'s: the level
byte, 10 to 25, the word stream's size in 8 bytes, then the coded stream.

**Memory.** 5,679,641,508 bytes at level 25 (5,416.5 MiB), under mcm's
5,961 MB. The layer is the same size at every level, about 136 MiB, so
level 10 is 168 MB where codec 7's is 26 MB.

## Files

```
docs/stages/stage-10.md      this brief; 10a's record, 10b's design
src/model/mix9.hpp           Mix12: the Layered parameter
src/codecs.cpp               codec 8
tests/                       Mix12's keys, hints and word length; codec 8's checksums and level byte
docs/figures/, write-up.md   the new point and the records
README.md, docs/roadmap.md, CLAUDE.md
```

The prototype's patches and timings live outside the repository, under
`H:\cpress-corpus\stage10\`.

## Exit criteria

- [ ] Section 1: every candidate's size on `e30`, and the combination's on
      `enwik8` and `enwik9`.
- [ ] Codec 8: `enwik9` with the source zipped under 144,054,338 bytes,
      round trip checked, with no more model memory than mcm's 5,961 MB.
- [ ] The Windows record: compress under 868 ns a byte, decompress under
      898, the machine quiet.
- [ ] Codecs 3, 5, 6 and 7 no slower than at `v1.3`.
- [ ] Tests: each new component's own; codec 8's level byte, limits and
      archive checksums.
- [ ] CI green, clang-format and clang-tidy clean, no compiler warnings.
- [ ] The write-up, README, roadmap and `cpress.hpp` updated.
- [ ] Tag `v1.4`, then a pull request freezing codec 8 with its golden
      archive and checksums.

## References

1. Matt Mahoney, *Large Text Compression Benchmark*, updated 17 November
   2026. <https://mattmahoney.net/dc/text.html>
