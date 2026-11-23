# Stage 9 brief: past mcm, codec 7

At `v1.2`, codec 6 (`words2`) makes 151,497,460 bytes of `enwik9`, and
151,610,174 with its source zipped: 43rd of the 238 programs in the Large
Text Compression Benchmark's main table of 17 November 2026 [1]. Just
above it:

| Rank | Program | Total, bytes | Compress ns/B | Decompress ns/B | Memory |
|---|---|---|---|---|---|
| 38 | drt\|lpaq9m | 144,054,338 | 868 | 898 | 1,542 MB |
| 39 | mcm 0.83 `-x11` | 144,934,149 | 394 | 281 | 5,961 MB |
| 40 | nanozip 0.09a | 149,328,821 | 1,149 | 1,141 | 32,000 MB |
| 41 | cadapt 0.12.0 | 150,273,521 | 1,385 | 1,748 | 661 MB |
| 42 | xwrt 3.2 | 151,223,933 | 2,537 | 2,328 | 1,691 MB |
| 43 | **cpress `words2`** | **151,610,174** | **293** | **366** | **4,236 MiB** |

The times are each tester's own; cpress's are its Windows record on the
i9-12900K. The benchmark underlines a time when no smaller program is
faster. `words2` is on the compression frontier: everything smaller
compresses more slowly. It is not on the decompression frontier, because
mcm, 4.4% smaller, decompresses faster.

**Under mcm's total, cpress would be on both frontiers.** It would place
39th. Every smaller program in the table is slower both ways: the
fastest of them, drt|lpaq9m, takes 868 and 898 ns a byte, 3.0 and 2.5
times `words2`'s. The size is the whole of the task; the time has room
to spend.

On this machine the picture is closer than the table's. Under WSL on 19
November, mcm ran at 334 and 347 ns a byte and `words2` at 313 and 424
(write-up, appendix, stage 8): cpress compresses 6% faster and
decompresses 22% slower.

What is known about the 4.4%, from reading mcm's source [2] (issue #4; GPL-3.0,
read and never copied):

- **The inputs are nearly ours.** mcm mixes orders 0 to 4, 6 and 8, a
  word, the word with the one before it, a context of recent character
  classes, and a match model. `Mix10` mixes orders 1 to 7, a word, a
  bigram, and a match model.
- **The mixer chooses its weights per bit,** from the last character's
  class, the word's length, the match's length and the partial byte, and
  its learning rate decays. Ours chooses per byte, from the previous byte
  and the match's length, and learns at a fixed rate (`kLearningShift`).
- **The tables are direct-mapped,** one state byte per context and bit, no
  check, one hash per context per byte. Ours are checked buckets of four,
  hashed per nibble (issue #19).
- **The dictionary is nearly codec 5's.** It ranks words by bytes saved
  rather than by count, uses all 128 high byte values, and keeps words of
  three letters or more seen nine times or more.
- **SSE only on the predicted-byte flag** and on bytes where it failed.
  Ours: two APMs on every bit, skipped where the mixer is sure (stage 8).
- **5,961 MB** of memory, against `words2`'s 4,236 MiB at level 25.

What is not known is which of these the 4.4% is made of. Stage 7 tried
two additions of its own, a second mixer and a third APM: −0.43% for 17%
more time, and left them.

**Exit:** codec 7 makes `enwik9` in under 144,934,149 bytes with its
source zipped, round trip checked. Its Windows record on this machine
compresses in under 394 ns a byte and decompresses no slower than
`words2`'s 366. The write-up and README say so; tag `v1.3`. If the
prototype (section 2) shows the size out of reach within that time, this
brief is amended with what is in reach, and why, before codec 7 is
written: the next mark is nanozip's 149,328,821, 40th.

## Scope

Four parts, in this order. The first two change no codec.

**9a. Where mcm's 4.4% is.**

1. mcm 0.83 built locally with each of its parts switched off in turn
   (section 1), and the size of the first 30 MB of `enwik8` (`e30`) for
   each. This ranks the differences listed above by what they are worth
   to mcm, before any is built here.

**9b. The prototype.**

2. A throwaway branch of `main`, patched behind environment switches as
   stage 8's was, so that one build tries every variant. The candidates
   are in section 2. Each is measured for size on `e30`; those that pay are
   combined and measured on `enwik8` and `enwik9`, then timed on a quiet
   machine.

**9c. The design.**

3. Codec 7's design written into this brief, from 9b's record, before its
   code: what it takes, its constants, its format, its memory.

**9d. Codec 7.**

4. Codec 7, built to make exactly the prototype's bytes, so that 9b's
   measurements stand (style guide, Measurement).
5. `enwik9`: the size, both speeds, peak memory and the source zip. The
   write-up and README updated; tag `v1.3`.

Out of scope:

- Huffman-shaped decisions (issue #4, plan item 3). It changes how every
  byte is coded and is the largest and riskiest idea on the list; mcm
  has it switched off.
- A block-parallel codec, threads, and hand-written SIMD.
- A neural mixer, or anything else at cmix's hundreds of times the time.
- Anything fitted to `enwik9`: constants chosen on it, or a dictionary
  shipped with the program.
- Any change to the output of codecs 1 to 6.
- Sending the results to the benchmark. That is the owner's, when content
  with cpress's performance (stage 6).

## Files

```
docs/stages/stage-9.md       this brief; 9a's and 9b's records, 9c's design
src/, include/, tests/       codec 7, its tests and its limits (9d; paths decided in 9c)
docs/figures/, write-up.md   the new point, a section for what codec 7 adds, the records
README.md, docs/roadmap.md, CLAUDE.md
```

The prototype's patches and timings live outside the repository, under
`H:\cpress-corpus\stage9\`, as stage 8's did.

## The choices

Each has a real trade-off. The owner took every recommendation on
2026-11-23, and the rest of the brief assumes them.

**Which target.** Getting under mcm's total takes 4.41% off `words2`'s
archive, once the source zip is counted; under nanozip's, 1.5%. Stage 7's
transform found 8.7% and stage 8's shortcuts 0.27%, so 4.4% from the
model is more than any change since the transform has given.
**Recommendation:** mcm's total, with nanozip's as the stated fallback.
Only the first changes what the table says about cpress: on both
frontiers, where today it is on one.

**What time to allow.** By the table's rule, anything under 868 ns a byte
compressing and 898 decompressing keeps cpress on both frontiers once it
is under mcm in size: 3.0 times `words2`'s time compressing and 2.5
times decompressing. The project's own standard is closer: on one
machine, mcm is the program to beat, and decompression is where it
leads. **Recommendation:** compress
under 394 ns a byte, stage 7's bar (mcm's listed time), and decompress no
slower than `words2`. Whatever the model adds to decoding has to be paid
back, which is what issue #19's layout is for.

**Whether to take mcm apart.** The list above says how mcm differs, not
what each difference is worth. Building mcm with its parts switched off,
one at a time, answers that for 30 MB of compressing per variant. It is
a local edit of a GPL-3.0 program, made and run on this machine for its
sizes. No line of it is committed, published or copied into cpress;
only the numbers are recorded, with the commit they came from.
**Recommendation:** do it. It is the cheapest way to know which
prototype to build first.

**How much memory.** mcm uses 5,961 MB; `words2` at level 25 uses
4,236 MiB, and level 26 as it stands would double the history tables and
the match model's buffer, to over 8 GiB. Stage 7 measured level 25
against 24 at −1.30% on `enwik9`, so more memory is the cheapest size
there is. The benchmark lists memory
but does not rank on it, and a compressor that needs more than 8 GB rules
out many machines that would otherwise run it. **Recommendation:** no
more than mcm's 5,961 MB. A larger table where it pays, the high orders,
fits under that; doubling everything does not.

**Where the time comes from.** Additions to the model cost time both
ways. Issue #19 measured what the decoder loses by waiting: given the
true bytes, `words2` decoded the 30 MB slice 17.8% faster, and the second
nibble's buckets alone were 8.2%. **Recommendation:** the table layout is
a 9b candidate like any other, measured for size and time, and taken if
the model's additions need the time it gives back.

## 1. mcm, taken apart

mcm 0.83 at `02b4e7d`, built by its own `make.sh` under WSL (GCC 13.3),
from a local copy patched with environment switches, on 23 November.
Each variant switches off or simplifies one part, then compresses `e30`
at `-x11` once; a size is a fact of the build, so one run is enough. With
every switch off, the copy makes the original's bytes: 5,787,924, against
`words2`'s 5,998,210, which is 3.63% more. Neither the patch nor any line
of mcm is in this repository.

An input switched off reads as 0 to the mixer, which then gives it no
weight, and its states are not updated. mcm's first input is order 0 while
no match runs and the match model while one does; they were switched off
separately.

| Variant | `e30`, bytes | Change |
|---|---|---|
| mcm 0.83 `-x11` | 5,787,924 | |
| **The mixer** | | |
| weights chosen as ours are: previous byte and match length, per byte | 5,919,123 | +2.27% |
| weights from the partial byte only | 5,916,626 | +2.22% |
| its weight sets per byte, not per bit | 5,900,709 | +1.95% |
| ours (previous byte and match length), but per bit | 5,820,301 | +0.56% |
| no character class in the choice | 5,832,167 | +0.76% |
| no word length in the choice | 5,808,054 | +0.35% |
| no match length in the choice | 5,803,569 | +0.27% |
| learning rate fixed at its floor (28) | 5,814,739 | +0.46% |
| learning rate fixed at 64 | 5,800,015 | +0.21% |
| never skips an update | 5,795,045 | +0.12% |
| skips the mixer, but always updates the states | 5,795,285 | +0.13% |
| **The inputs** | | |
| the character-class context off | 5,903,669 | +2.00% |
| order 2 off | 5,842,302 | +0.94% |
| order 4 off | 5,838,885 | +0.88% |
| order 6 off | 5,831,570 | +0.75% |
| order 3 off | 5,827,417 | +0.68% |
| order 1 off | 5,825,899 | +0.66% |
| the word off | 5,820,579 | +0.56% |
| the word pair off | 5,809,303 | +0.37% |
| order 0 off | 5,801,136 | +0.23% |
| the match model off | 5,800,849 | +0.22% |
| order 8 off | 5,797,517 | +0.17% |
| orders 5 and 7 in place of 6 and 8 | 5,780,003 | −0.14% |
| **The rest** | | |
| SSE off | 5,791,668 | +0.06% |
| the predicted-byte flag off (`-lzp=false`) | 5,787,245 | −0.01% |
| no dictionary (`-filter=none`) | 5,864,393 | +1.32% |
| dictionary ranked by count only | 5,787,022 | −0.02% |
| dictionary words seen 20 times or more | 5,784,075 | −0.07% |

What it says, in order of size:

1. **The mixer's weight sets are chosen per bit.** Chosen per byte, mcm
   loses 1.95%. Chosen as ours are, per byte, it loses 2.27%. Per bit, ours
   loses only 0.56%, so the partial byte in the choice is most of it.
2. **The character-class context is worth 2.0%,** more than any order.
   Each byte maps to one of 16 classes (letters, digits, space,
   punctuation and so on) and the context is the hash of the last eight
   classes: it says what kind of text this is, not which text.
3. **A learning rate that decays** from fast to slow is worth 0.2 to 0.5%.
4. **The rest is small:** SSE 0.06%, the update skip 0.12%, the flag
   nothing at all. mcm's dictionary is worth 1.3% to mcm, where codec 5's
   transform is worth 8.7% to ours (stage 7), and ranking it by count
   rather than bytes saved changes nothing; T1 to T3 are not where the gap
   is.

A fixed rate at mcm's starting value, 382, diverged (271% larger) and is
left out of the table.

## 2. The prototype

*To be filled in by 9b.*

The candidates, in the order 9b tries them unless section 1 reorders
them. Each changes `words2`'s predictions; each is measured for size on
`e30`, and the ones that pay are timed.

| | Candidate | Where it comes from |
|---|---|---|
| M1 | Weight sets chosen per bit, from character class, word length, match length and the partial byte | mcm |
| M2 | A learning rate that starts high and decays | mcm |
| M3 | Order 0 as an input; orders 6 and 8 in place of 5 and 7 | mcm |
| M4 | A context of recent character classes (letter, digit, space, punctuation, code byte) | mcm |
| M5 | Larger tables for the high orders, within mcm's memory | the memory choice |
| T1 | The dictionary ranked by bytes saved; all 128 high byte values; words of 3+ letters seen 9+ times | mcm; issue #4, plan item 4 |
| T2 | A code for a word with the space before it | issue #4, plan item 4 |
| T3 | Codes for frequent strings that are not words (`[[`, `]]`, `&quot;`) | issue #4, plan item 4 |
| S1 | Orders 1 and 2 indexed directly, one 272-byte block per context, fetched once the byte is one of two | issue #19, untested |
| S2 | Orders 5 to 7 in line pairs | issue #19: +0.04%, decode −2.7% |

How they are judged:

- **Size** on `e30`, against `words2`'s 5,998,210. A candidate that does not
  make `e30` smaller is not timed, unless its purpose is speed (S1, S2).
- **Time** on a quiet machine only: `cpress-bench --cpu 2`, configurations
  interleaved, the fastest of several rounds, as stage 8's section 4.
- **The combination** on `enwik8`, then `enwik9`, with every round trip
  checked. `enwik8` is not used to choose constants, so it tests whether
  `e30`'s choices hold on a file three times larger; `enwik9` is used for
  nothing but the record.

## 3. Codec 7

*To be written in 9c, from section 2's record.*

Codec 7 is `words2`'s transform and payload, with what section 2 takes.
Its name is decided with its design.

## Exit criteria

- [ ] Section 1: mcm's parts ranked by what they are worth on `e30`.
- [ ] Section 2: every candidate's size on `e30`; the combination's sizes
      on `e30`, `enwik8` and `enwik9`; its times on a quiet machine.
- [ ] Section 3 written, and this brief amended if the target moved.
- [ ] Codec 7 makes the prototype's bytes on `e30` and `enwik8`.
- [ ] `enwik9` with the source zipped under 144,934,149 bytes (or the
      amended target), round trip checked.
- [ ] Tests: each new component's own; the level byte and the limits of
      codec 7; the golden archives and checksums of codecs 1 to 6,
      unchanged.
- [ ] CI green, the fuzz job included.
- [ ] clang-format and clang-tidy clean, no compiler warnings.
- [ ] The Windows record: compress under 394 ns a byte, decompress no
      slower than `words2`'s 366.1, on `enwik9`; codecs 3, 5 and 6 no
      slower than at `v1.2` on `e30`, if the driver or ContextMix changed.
- [ ] The write-up, README, roadmap and `cpress.hpp` updated.
- [ ] Tag `v1.3`, then a close-out pull request freezing codec 7 with its
      golden archive and checksums, as #18 and #21 did.

## What review will look at

- Codecs 3, 5 and 6 are unchanged: their golden archives pass, and
  anything new in ContextMix is behind a template parameter they do not
  set.
- Each choice in section 3 has a size from section 2 behind it, measured
  on `e30` and checked on `enwik8`, never chosen on `enwik9`.
- The decoder's memory stays in proportion to the level and the bytes
  present, and the level's ceiling is checked before anything is
  allocated.
- Nothing of mcm's source is in the repository.

## References

1. Matt Mahoney, *Large Text Compression Benchmark*, main table and
   program notes, 17 November 2026:
   <https://mattmahoney.net/dc/text.html>.
2. Mathieu Chartier, mcm, <https://github.com/mathieuchartier/mcm>,
   version 0.83 at commit `02b4e7d` (5 April 2015).
