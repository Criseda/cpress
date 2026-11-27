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
compresses in under 394 ns a byte and decompresses in under 898, the
fastest smaller program's time, so that it is on both frontiers. The
write-up and README say so; tag `v1.3`.

*Amended on 27 November, from section 2.* The exit first asked for
decompressing no slower than `words2`'s 366. The prototype found nothing
that gets under mcm's total at that speed: every table the size needs
costs decoding time, and the decoder's wait for memory, not the model's
arithmetic, is most of the difference (section 2). The owner kept the
size and allowed the time: codec 7 is expected to decompress about 14%
slower than `words2`, near 415 ns a byte, against mcm's 347 on this
machine. Paying that back is left for after the freeze, where it changes
no byte.

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

A throwaway branch of `main` (local, never pushed), `ContextMix` patched
behind environment switches that change only `Mix10Flag`; with every
switch off it makes `words2`'s bytes. Patches and logs are in
`H:\cpress-corpus\stage9\proto\`. Run on 23 November.

**Sizes on `e30`,** each step on top of the one before:

| Step | `e30`, bytes | Change from `words2` |
|---|---|---|
| `words2` | 5,998,210 | |
| M4, a class context: the last 12 byte classes (16 classes, my own grouping, below) | 5,881,200 | −1.95% |
| M1, weight sets per bit: class of the last byte × word length × match length × partial byte | 5,805,116 | −3.22% |
| M2, learning rate from 218/16 of the base, down by 1/16 per update to the base | 5,792,910 | −3.42% |
| a bias input, a constant 256 to the mixer | 5,782,379 | −3.60% |
| APM 1 keyed by the last two classes as well as the partial byte | 5,775,660 | −3.71% |
| a second class context, the last 6 classes | 5,743,494 | −4.25% |
| weight sets by the classes of the last two bytes, not class × word length | 5,726,540 | −4.53% |

Alone, against `words2`: M4 −1.95%; M1 −1.12% (class choice) or −0.63%
(`words2`'s choice with the partial byte); M2 −0.02%, since `words2`'s 1,024
weight sets each see millions of updates and a fast start matters only
when there are 262,144. Tried and not taken:

| Tried | Change on `e30` | Why not |
|---|---|---|
| a second mixer, weights chosen as `words2`'s per bit, and a final mixer of the two | −0.19% | 25% more time |
| a third APM, order 2 hashed to 12 bits × partial byte | −0.17% | 13% more time |
| table and map updates skipped when the mixer's error is under 10/4096 | −0.06% | +0.23% on `enwik9` |
| the class contexts' length: 10 and 5, 14 and 6, 16 and 7 | within 0.2% of 12 and 6 | |
| the class context also keyed by the last byte | +0.31% | |
| T1 to T3 | not tried | section 1: mcm's dictionary differences are worth nothing to it |

The classes, one per byte value: 0, control bytes (which include the
transform's flags and escape); 1, line feed; 2, space; 3, digits; 4,
lower case; 5, upper case; 6, 128 and up (the transform's codes and
UTF-8); 7, `. ! ?`; 8, `, ; :`; 9, quotes; 10, parentheses; 11, square
brackets; 12, `{ } |`; 13, `< > / \`; 14, `= & # *`; 15, the rest.

**The orders, on `enwik9`.** Each of orders 6 and 7 is worth under 0.1%
on `e30`, but not on `enwik9`:

| Byte orders | `enwik9`, bytes |
|---|---|
| 1 to 7 | 144,212,605 |
| 1 to 6 | 145,280,396 (+0.74%) |
| 1 to 5 | 146,655,682 (+1.69%) |

With orders 1 to 7, `enwik8` is 18,009,934 (−4.40%; mcm 18,234,412) and
`enwik9` 144,212,605 (−4.81%), round trip checked. With the source zip,
about 115,000 bytes, that is 144.33 million, under mcm's 144,934,149.

**The class tables' size, on `enwik9`.** At the level's 2^25 slots each,
the two class tables add 1,024 MiB, and the per-bit weights and APM 1's
larger table 20 MiB, for about 5,280 MiB in all, under mcm's 5,961 MB
(5,685 MiB).
At 2^22 slots: 144,445,615 (+0.16%); at 2^20: 144,839,413 (+0.43%).

**Time, on `enwik9`.** `cpress-bench --cpu 2 --repeat 1`, Windows, the
machine otherwise idle, each configuration once, interleaved with
`words2` built from `main`. The prototype's weight rows are aligned to a
cache line and the next bit's row is prefetched; neither changes a byte.

| Configuration | `enwik9`, bytes | Compress, ns/B | Decompress, ns/B |
|---|---|---|---|
| `words2` | 151,497,460 | 291.4 | 371.9 |
| everything above but the class tables (9 tables, as `words2`) | 148,206,844 (−2.17%) | 322.8 (+11%) | 401.9 (+8%) |
| one class table, the last 12 classes (10 tables) | 144,553,419 (−4.58%) | 367.1 (+26%) | 448.7 (+21%) |
| both class tables (11 tables) | 144,212,605 (−4.81%) | 382.9 (+33%)* | 458.6 (+27%)* |

\* In the round before, against `words2` at 288.7 and 362.2.

`e30` misled about time. There each table is half a gigabyte of pages
touched for the first time, and those faults dominate. On `enwik9`, the
12-class table costs about 12% of `words2`'s decoding time and the 6-class
table about 3%: it has far fewer contexts, so more of it stays in cache.
The other changes cost 8%. A profile of the decoder on `enwik8` puts the
model's own time in the tables' bucket search (11%), the prefetch loops
(15%) and the maps and the mixer.

Tried for time, without effect on `enwik9`: prefetching APM 1's next
entry, and class tables of 2^22 slots, which also cost 0.16% in size.

**What it means for the exit.** The size is in reach: with one class
table, 144.55 million plus the source zip is about 260,000 bytes under
mcm's total, and with two, about 600,000. The time is not. Compressing
stays under 394 ns a byte, but decompressing is 21 to 27% slower than
`words2`, against an exit that asks for no slower. By the benchmark's own
rule the result would still be on both frontiers, since every smaller
program decompresses at 898 ns a byte or slower. But it is a step back
from `words2`, and further behind mcm's 281.

That was the owner's decision. The choices, as put on 24 November:

1. **Keep the size target and allow the decode time:** one class table,
   decompressing about 21% slower than `words2`, or two, about 27% slower.
   9c then writes the design.
2. **Keep the time bar and lower the target:** nothing measured here
   gets past mcm without a new table layout first. Issue #19's
   hash-once-per-byte layout, or something like it, would come first, as a
   stage of its own, and is unmeasured.
3. **Keep the time bar and take nanozip's mark:** without the class
   tables, 148.2 million passes nanozip's 149.3 million (40th). Even that
   decompresses 8% slower than `words2`.

**Fewer tables for less time,** 23 to 26 November, on `enwik9`. A
table's cost turned out to be mostly its work on each bit: the slot it
claims, its state map, its mixer input and its update. Filing order 6 in
order 5's cache line (`share6`), so that one fetch serves both, kept half
of order 6's worth and saved little time; dropping a table saved more.
The machine was not quiet, so the times are 4 to 8% high; each variant was
interleaved with `words2` in the same run, so the ratios hold. Every
round trip was checked.

| Tables | `enwik9`, bytes | Decompress, × `words2` |
|---|---|---|
| `words2` | 151,497,460 | 1.00 |
| all of the above | 144,212,605 | 1.26 to 1.31 |
| order 6 in order 5's line | 144,471,805 | 1.25 |
| **no order 6** | **144,613,104** | **1.14** |
| orders 5 and 7 in order 4's and 6's lines | 145,261,525 | 1.23 |
| no orders 5 and 7 | 145,663,999 | 1.05 |

Without order 6, `enwik8` is 18,033,918 (`e30` 5,729,199), and `enwik9`
with the source zip about 144,725,800: 208,000 bytes under mcm's total.
From the ratios and `words2`'s quiet record, compressing should take about
336 ns a byte and decompressing about 415.

**Why decoding is slower than coding.** The encoder knows every byte and
prefetches each table's buckets a byte ahead; the decoder knows a byte's
bits as it decodes them. Given the true bytes in a timing-only build
(`P_ORACLE`), the prototype decoded `enwik8` within 2 to 5% of its
compressing time. Guessing the bytes instead (the match's guess, then the
byte that last followed the same three or four bytes) was slower than
not guessing: a wrong guess fills the line fill buffers with fetches no
one reads, and a profile (VTune, 6 events) put the stalls there and in
the loops that issue the fetches. Fetching the second nibble's keys by
remarking the first nibble's, rather than hashing them again, changes no
byte and saved about 1% in the prototype. In codec 7's build it made no
clear difference to codec 7 and made codecs 3, 5 and 6 3 to 5% slower
compressing on `e30`, so it was left out (section 3).

**The decision,** 27 November: option 1 without order 6. The size stays
the target, decompressing about 14% slower than `words2` is allowed, and
section 3 is the design.

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

Codec 7, `words3`, is `words2`'s transform and payload with a new model,
`Mix11`: `Mix10Flag` with byte classes and without order 6. It makes the
prototype's bytes (section 2, "no order 6"), so the prototype's sizes
stand for it: `e30` 5,729,199, `enwik8` 18,033,918, `enwik9`
144,613,104.

**What it takes,** each with its size from section 2:

| Part | What it is | Constants |
|---|---|---|
| Byte classes | sixteen, one per byte value, as listed in section 2 | `byte_classes.hpp` |
| Class tables | two more history tables, keyed by the classes of the last 12 bytes and of the last 6, each at the level's size | `kLongClasses`, `kShortClasses` |
| Weight sets per bit | chosen by the classes of the last two bytes, the match's length class and the partial byte: 16 × 16 × 4 × 256 = 262,144 sets | |
| Decaying rate | each set learns at 218/16 of the base rate at first, a sixteenth less with each update, down to the base | `kRateInitial`, `kRateFinal` |
| Bias | a constant input of one unit of ln odds | `kBiasStretch` |
| APM 1 | keyed by the classes of the last two bytes as well as the partial byte | |
| Order 6 | skipped: orders 1 to 5 and 7 | `SkippedOrder` |

The rest is `words2`'s: word and bigram tables, the match model, the
flag, APM 2, the shortcuts and their constants. The decaying rate also
reaches the flag model's mixer, because the prototype's switch for it
changed every mixer; without it, `e30` is 8 bytes larger. It was not
measured alone, and is kept so the prototype's sizes stand.

**Where it lives.** `ContextMix` takes two more template parameters,
`Classes` and `SkippedOrder`, and `Mixer` and the flag model one,
`DecayingRate`; codecs 3, 5 and 6 set none of them, and their golden
archives and checksums are unchanged. The classes and their keys are
`src/model/byte_classes.hpp`. With `DecayingRate`, a mixer's set and its
rate share one 64-byte line, and the next bit's set is prefetched; both
were in the prototype and change no byte.

`ByteContext` carries a class history in every model; without classes
it stays 0. Removing it from the other models, through an empty base,
made no difference to their speed on `e30`.

**Format.** Id 7, name `words3`. The payload is `words2`'s: the level
byte, 10 to 25, the word stream's size in 8 bytes, then the coded stream
with the flag. The same limits hold, and the level is checked before
anything is allocated.

**Memory.** `Mix11::memory_bytes()`: 24.4 MiB at level 10, 4,768.5 MiB at
25 (5,000,147,152 bytes), against `words2`'s 4,236.4 MiB and mcm's 5,685
MiB. One history table more than `words2` (two class tables, less order
6), 16 MiB of weight sets and 4 MiB for APM 1. The decoder also holds the
word stream, as `words2`'s does.

**Not taken,** from section 2's record: the second mixer and the third
APM (time), skipping the tables' updates (worse on `enwik9`), the class
context keyed by the last byte (larger), smaller class tables (larger,
and no faster), shared cache lines (little time for their size), and
guessed prefetching in the decoder (slower).

## Exit criteria

- [x] Section 1: mcm's parts ranked by what they are worth on `e30`.
- [x] Section 2: every candidate's size on `e30`; the combination's sizes
      on `e30`, `enwik8` and `enwik9`; its times on a quiet machine.
- [x] Section 3 written, and this brief amended if the target moved.
- [x] Codec 7 makes the prototype's bytes on `e30` and `enwik8`.
- [ ] `enwik9` with the source zipped under 144,934,149 bytes (or the
      amended target), round trip checked.
- [ ] Tests: each new component's own; the level byte and the limits of
      codec 7; the golden archives and checksums of codecs 1 to 6,
      unchanged.
- [ ] CI green, the fuzz job included.
- [ ] clang-format and clang-tidy clean, no compiler warnings.
- [ ] The Windows record: compress under 394 ns a byte, decompress under
      898, on `enwik9`; codecs 3, 5 and 6 no slower than at `v1.2` on
      `e30`, if the driver or ContextMix changed.
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
