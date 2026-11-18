# Stage 7 brief: text, toward the frontier

At `v1.0`, cpress makes 165,953,692 bytes of `enwik9`, and 166,051,214
with its source zipped: 61st of the 238 programs in the main table of
the Large Text Compression Benchmark on 17 November 2026, and on none of
its frontiers [1]. The benchmark underlines a time when no smaller
program is faster. Near cpress, the compression-time frontier is:

| Program | Total, bytes | Compress ns/B | Decompress ns/B |
|---|---|---|---|
| drt\|lpaq9m | 144,054,338 | 868 | 898 |
| mcm 0.83 `-x11` | 144,934,149 | 394 | 281 |
| bsc-m03 0.4.0 | 160,364,392 | 160 | 135 |
| **cpress 1.0** | **166,051,214** | **454** | **561** |

Each row is timed on its own tester's machine; cpress's on an i9-12900K.
There are two ways onto that frontier for compression:

- **A:** a total under bsc-m03's 160,364,392, compressing faster than
  mcm's 394 ns a byte. That is 3.4% smaller and 13% faster.
- **B:** a total under mcm's 144,934,149 at today's speed: 12.7% smaller.

Stage 7 aims at A. Decompression is not its target: it would have to be
under 281 ns a byte as well, twice today's speed, and the only route
measured to that, independent blocks, costs size (issue #4, part 2).

What the programs ahead of cpress did, from the benchmark's own records:

- **A dictionary transform.** Rhatushnyak's `drt` replaces words with
  short codes before the compressor sees them. On `enwik8` it makes lpaq1
  4.5% smaller, lpaq9m 5.2% and paq8px 5.2% [1]. mcm went from 156,544,880
  bytes of `enwik9` at 0.82 to 146,525,446 at 0.83 with about the same
  memory (`-10 -max` against `-x10`) [1], and the commits between the two
  add a dictionary built from the input's own words [2]. A transform also
  leaves the model fewer bytes to code, which is the one change that could
  make cpress smaller and faster at once.
- **More model at the same speed.** lpaq1 to lpaq8 is 164,508,919 to
  161,987,713 bytes of `enwik9`, 1.5%, at 3,646 and 3,682 ns a byte on
  the same machine [1]. Stage 3 measured two additions and left them: an
  order-7 history table, −0.8% on `enwik8`, and a third APM, −0.2%
  ([stage 3](stage-3.md), "Measured, worth having").
- **Memory that grows with the file.** `mix9` is 0.3% under lpaq1 on
  `enwik8` and 0.9% over it on `enwik9`, where its fixed 980 MiB is
  spread over ten times the data and lpaq1 has 1.5 GB.

The plan is the project's usual one: measure, prototype, then build.
Stage 7 measures mcm on this machine and profiles `mix9` before it changes
anything, prototypes the transform outside the repository, and only then
designs codec 5.

**Exit:** codec 5 makes `enwik9` in under 160,364,392 bytes in all,
compressing in under 394 ns a byte on this machine, round trip checked,
and the write-up and README say so. If the prototype (section 3) shows A
out of reach, this brief is amended with what is in reach, and why,
before codec 5 is written.

## Scope

Four parts, in this order. The first two change no codec.

**7a. mcm, measured here.**

1. `bench/text-baselines.sh --mcm PROGRAM`: mcm 0.83 at `-x11`, beside
   `mix9`, in one sitting on `enwik8`, then once each way on `enwik9`.

**7b. The profile.**

2. A sampling profile of `mix9` compressing and decompressing `enwik8`,
   attributed by source line and by component, completing issue #4's
   table. Changes that keep codec 3's bytes may land here, each measured.

**7c. The prototype.**

3. A word transform built from the input's own words, outside the
   repository, with `mix9` behind it, on `enwik8` and `enwik9`: the sizes,
   the dictionary's cost, and the speed per input byte both ways.

**7d. Codec 5.**

4. The transform and the model as codec 5, with whatever of a larger
   level and the order-7 table section 3 shows is worth its time. Its
   design is written into this brief after section 3, before its code.
5. `enwik9`: the size, both speeds, peak memory and the decompressor's
   zip. The write-up and README updated; tag `v1.1`.

Out of scope:

- The decompression frontier, and with it blocks and threads (issue #4,
  part 2) and a table layout for the decoder (part 3).
- Anything fitted to `enwik9`: constants chosen on it, a dictionary built
  from Wikipedia and shipped with the program. The benchmark allows them;
  a model of text should not need them.
- Hand-written SIMD, unless the profile shows the mixer is far more of the
  time than issue #4's measurements say (at most 98 of 453 ns a byte).
- Any change to the output of codecs 1 to 4.
- Sending the results to the benchmark. That is the owner's, when content
  with cpress's performance (stage 6).
- A neural mixer.

## Files

```
docs/stages/stage-7.md       this brief; 7a's and 7b's results, 7c's design
bench/text-baselines.sh      --mcm PROGRAM
src/, include/, tests/       codec 5, its tests and its limits (7d; paths decided then)
docs/figures/, write-up.md   the new point, the transform's section, the records
README.md, docs/roadmap.md, CLAUDE.md
```

## The choices

Each has a real trade-off; the recommendation is what the rest of the
brief assumes.

**Which target.** A needs 3.4% in size and 13% in time; B needs 12.7% in
size, about what lpaq9m and `drt` took a year of versions to find over
lpaq1. **Recommendation:** A. B is a direction for later stages.

**Which clock.** The benchmark compares times from different machines:
mcm's 394 ns is from another tester's, bsc-m03's from a 5 GHz i7-9700K,
cpress's from an i9-12900K. A listing is judged by the table's own rule,
so the exit is too: cpress under the 394 the table lists, on this
machine, by the same measure as `v1.0`'s `enwik9` record. **Recommendation:**
that exit, and section 1 measures mcm here so that the write-up can also
say, truthfully, how the two compare on one machine. If mcm turns out much
faster here than 394, the write-up says so; the exit does not move.

**Where mcm runs.** The stage 6 sitting ran in WSL with GCC, every program
pinned to one logical processor, and `v1.0`'s `enwik9` record on Windows
with MSVC. mcm builds with g++ and a shell script. **Recommendation:**
WSL, beside a `mix9` built there from the same commit, so that the
comparison is one compiler and one system. The exit's own number stays a
Windows record, as `v1.0`'s was, since that is what a listing would carry.

**The profiler.** Every model part inlines into one loop, so a profile by
function says nothing; it has to be by source line. `perf` samples by
line and prints text; under WSL it may have only a timer to sample by, no
hardware counters, which still attributes time. Visual Studio's sampler
needs no code change but is read in the IDE. callgrind counts
instructions, not time, and would miss what the cache costs.
**Recommendation:** `perf` under WSL, on a GCC build with debug
information; if it cannot run there, callgrind for the instruction counts
and Visual Studio's sampler, opened by the owner, for the time.

**Where the dictionary comes from.** `drt` ships an English dictionary
with the program; mcm builds one from the input [2]. A shipped dictionary
costs nothing in the archive and is ready before the first byte, but it
is a fit to one language, its size counts in the decompressor's zip, and
built from Wikipedia it would be a fit to the benchmark's file.
**Recommendation:** from the input. Compression reads the input twice,
once to count words and once to code, and the archive carries the
dictionary; section 3 measures what that costs.

**Where the transform sits.** It can be a reversible step before the
model, which then codes the transformed bytes as it codes any others, or
it can be part of the model, which predicts words. **Recommendation:**
before the model, unchanged. It isolates what the transform is worth,
keeps one model, and is how `drt` and the word-replacing transforms in
the literature work [3]. Section 3 measures whether `mix9`'s word
contexts, which hash letters, still earn their place when many words are
codes.

## 1. mcm, measured here

**The program.** mcm by Mathieu Chartier, GPL-3.0, from its repository
[2] at `02b4e7d`, the last commit of 5 April 2015, the date the benchmark
gives for 0.83's release [1]. The repository has no tags, so the sizes
check the commit: at `-x11` mcm must make the benchmark's 18,233,295
bytes of `enwik8` and 144,854,575 of `enwik9`. A different size means a
different program, and is explained before any time is recorded. It is
built with its own `make.sh`, and run, not read: no code of it comes into
cpress.

**The script.** `bench/text-baselines.sh` gains `--mcm PROGRAM`, as it has
`--bsc` and `--lpaq1`: mcm 0.83 at `-x11`, which the benchmark lists at
5,961 MB of memory. It also gains `--only NAMES`, to run some of its
programs and not all: the six it always runs would add about an hour of
compressing to an `enwik9` sitting, for rows stage 6 already has on
`enwik8`.

**The sitting.** On a quiet machine, every program on logical processor
2:

- `enwik8`: `cpress-bench --cpu 2 --repeat 3` for `mix9`, built in WSL at
  this brief's commit, and `text-baselines.sh --mcm PROGRAM --only mcm
  --repeat 3`;
- `enwik9`: the same, at `--repeat 1`.

The scratch directory is `/dev/shm`, which is memory, so that no file is
read through a 9P mount and WSL's disk image does not grow: `enwik9`, its
archive and the restored copy are 2.2 GB, beside mcm's 6 GB, inside
WSL's 15 GB.

**What it records.** The rows go into the commit message and into this
section. 7a changes no codec.

**Results,** 18 November 2026, WSL 2 (Linux 6.18) on the i9-12900K, GCC
13.3, logical processor 2, every round trip checked; `mix9` at `efe7d14`
from a clean clone, mcm built by its `make.sh`:

| | Bytes | Compress ns/B | Decompress ns/B | Peak MiB, each way |
|---|---|---|---|---|
| `enwik8`, `mix9` | 19,687,838 | 439.7 | 565.8 | 1,100 / 1,196 |
| `enwik8`, mcm `-x11` | 18,234,412 | 377.1 | 373.4 | 5,769 / 5,666 |
| `enwik9`, `mix9` | 165,953,692 | 431.9 | 534.5 | 2,098 / 3,051 |
| `enwik9`, mcm `-x11` | 144,854,575 | 333.8 | 346.9 | 5,856 / 5,684 |

`enwik8` is the fastest of three runs each way, `enwik9` one. `mix9`'s
peaks are `cpress-bench`'s, which holds the input, the archive and the
restored copy at once; mcm's are GNU time's.

- **mcm is the program the benchmark measured:** its `enwik9` is the
  benchmark's 144,854,575 to the byte. Its `enwik8` is 1,117 bytes (0.006%)
  over the benchmark's 18,233,295, which this sitting does not explain;
  the benchmark's note says `-x11` was tested from "optimized source with
  comments removed", so the build behind that row may differ from this
  commit's.
- **On one machine, mcm is ahead on every count:** 12.7% smaller on
  `enwik9`, 23% faster compressing and 35% faster decompressing. Its
  listed 394 ns a byte is slower than it runs here. The exit stays the
  table's rule, as the choices say, and the write-up will give both.
- **`mix9` in WSL with GCC is faster than `v1.0`'s Windows record**
  (431.9 and 534.5 against 454.2 and 561.4 on `enwik9`). The exit's
  number is the Windows record, so these do not count towards it.
- `cpress-bench` printed `efe7d14-dirty`: it reads the commit from the
  directory it runs in, which was the Windows tree with this section's
  script change uncommitted; the binary was built from the clean clone,
  and its archives are `v1.0`'s byte for byte.

## 2. The profile

**What it answers.** Issue #4 left about 280 of `mix9`'s 453 ns a byte,
compressing at level 12, unattributed: state maps, slot selection, the
match model and word hashing together. This section attributes them, and
the decoder's time too.

**How.** `mix9` at `v1.0`, built by GCC as Release with debug
information added, compressing and
decompressing `enwik8` under `perf record`, reported by source line and
summed by component. One run each way is enough: a profile is a
proportion, not a speed.

**What it produces.** A table of where the time goes, each way, and a
list of changes that keep codec 3's output, each with the time it could
save. Any of them that lands in this stage is measured on a quiet machine
on `enwik8` and keeps every golden archive; it helps codec 5 as much,
since codec 5 shares the model.

## 3. The prototype

**What it is.** A throwaway C++ program, outside the repository, as stage
5's prototype was: it transforms a file and transforms it back. `cpress`
at `v1.0` compresses the transformed file with `mix9`, unchanged. The
total is the archive plus the dictionary, compressed.

**The transform.** The word-replacing transforms in the literature [3]
agree on the parts; the prototype measures each rather than taking them
on trust:

- A word is a maximal run of letters. A word with a capital first letter,
  or all in capitals, is coded as its lower-case form after a flag byte.
- The dictionary is every word seen at least *n* times, ordered by
  frequency so that the commonest words have the shortest codes.
- A code is one, two or three bytes, from byte values that are rare in
  the input, with an escape before any literal byte that collides with
  them. `enwik8` is UTF-8, so the bytes above 127 are not free; which
  values to use is measured.

**What it measures,** on `enwik8`, then on `enwik9`:

| Measure | Why |
|---|---|
| Transformed size, against the input | How many fewer bytes the model codes |
| `mix9`'s archive of it, plus the dictionary compressed | The size against `mix9` alone |
| Transform and model time, per input byte, each way | The speed against `mix9` alone |
| The same with the word contexts' hashes fed codes | Whether `mix9`'s word model still pays |
| *n*, and the dictionary's size | Where the dictionary stops paying |

**The decision.** If `enwik9` is at least 3.4% smaller in all, and the
time per input byte falls, codec 5 follows its design. If not, this brief
is amended with what the measurements allow.

## 4. Codec 5

Written after section 3, before code. What is already fixed:

- One predictor both ways, integer only, the standard library only, as
  every codec.
- The dictionary in an archive is untrusted input: its count, its words'
  lengths and its codes are checked before anything is built from them,
  and a corrupt one fails as a corrupt archive does.
- The archive states its memory, as codecs 3 and 4 do; the decoder
  allocates what that says, whatever the original size.
- A golden archive once the codec has shipped, as `docs/style.md` asks.

## Exit criteria

- [x] 7a: mcm 0.83 `-x11` and `mix9` on `enwik8` (fastest of 3) and
      `enwik9` (one run) in one sitting, mcm's sizes equal to the
      benchmark's; the rows in the commit message and in section 1.
      *`enwik9` equal; `enwik8` 0.006% over, unexplained (section 1).*
- [ ] 7b: the profile's table, both ways, in section 2 and issue #4.
- [ ] 7c: the prototype's measurements, and codec 5's design, in this
      brief.
- [ ] Codec 5 makes `enwik9` in under 160,364,392 bytes in all, archive
      and decompressor zip, compressing in under 394 ns a byte, on this
      machine, with the `cpress-bench` row in the commit message.
- [ ] Codec 5's tests, its untrusted-input tests among them.
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan: CI on
      the closing commit is the record.
- [ ] clang-format clean, clang-tidy clean, no compiler warnings.
- [ ] The write-up (the transform's section, the new point, the records),
      the README, `cpress.hpp`, `docs/roadmap.md` and `CLAUDE.md` updated;
      tag `v1.1`.

## Suggested commits

1. This brief, and the roadmap, README and `CLAUDE.md` pointing at it.
2. 7a: `--mcm`, with the sitting's rows in the commit message.
3. 7b: the profile in this brief, and each change that keeps codec 3's
   bytes, measured.
4. 7c: the prototype's measurements and codec 5's design in this brief.
5. 7d: codec 5 and its tests.
6. The `enwik9` record, the write-up, the README; tag `v1.1`.

## What review will look at

- Did mcm make the benchmark's sizes, and were both programs timed the
  same way, on the same processor, in one sitting?
- Is every number in section 2 a proportion of one run, and every claimed
  saving measured, not estimated?
- Does the prototype reverse every file it transforms, including files
  that are not text and files with every byte value?
- Is anything in codec 5 chosen on `enwik9`?

## References

1. Matt Mahoney, *Large Text Compression Benchmark*, main table and
   program notes, 17 November 2026:
   <https://mattmahoney.net/dc/text.html>.
2. Mathieu Chartier, mcm, <https://github.com/mathieuchartier/mcm>:
   commits `4500ea7` "[WIP] Add dict + cleanup" (15 March 2015),
   `7c24f85` "Actually write dictionary" and `371d8e8` "Add inverse
   dictionary" before `02b4e7d` (5 April 2015).
3. P. Skibiński, Sz. Grabowski and S. Deorowicz, "Revisiting
   dictionary-based compression", *Software: Practice and Experience*
   35(15), 1455–1476, 2005, <https://doi.org/10.1002/spe.678>.
