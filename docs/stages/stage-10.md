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

**What codec 8 is for.** **Recommendation:** past lpaq9m in size, with
the time frontiers kept and lpaq9m left behind on time as well: under
144,054,338 bytes of `enwik9` with the source zipped, compressing under
868 ns a byte and decompressing under 898. That is the one move that
changes three underlines at once (cpress's place, and lpaq9m's two), and
a size is the same on any machine, where a time is each tester's own.

**Memory.** The memory frontier and lpaq9m's size pull opposite ways with
this table layout: every halving costs more than the 0.47% the size
target needs. **Recommendation:** not the memory frontier in this stage.
No more model memory than codec 7's 4,756 MiB, and a driver that streams
rather than holds the input, the word stream and the output, which
changes no byte and brings the listed memory to about the model's,
under mcm's 5,961 MB. The frontier itself comes back with a denser table
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

## Files

```
docs/stages/stage-10.md      this brief; 10a's record, 10b's design
src/, include/, tests/       codec 8, its tests and its limits (paths decided in 10b)
docs/figures/, write-up.md   the new point and the records
README.md, docs/roadmap.md, CLAUDE.md
```

The prototype's patches and timings live outside the repository, under
`H:\cpress-corpus\stage10\`.

## Exit criteria

- [ ] Section 1: every candidate's size on `e30`, and the combination's on
      `enwik8` and `enwik9`.
- [ ] Codec 8: `enwik9` with the source zipped under 144,054,338 bytes,
      round trip checked, with no more model memory than codec 7's.
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
