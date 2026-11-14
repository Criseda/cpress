# cpress

A lossless compressor built on context mixing, written from scratch in C++20.

**Status: stage 4 done, tagged `v0.4`; stage 5 is under way.**
The container format, command-line tool, tests and benchmark runner work end
to end. There are four codecs for bytes: `stored`, which does not compress;
`order0`, a binary arithmetic coder driven by an order-0 bitwise model;
`mix6`, which drives the same coder with six hashed context models, orders 1
to 6, combined by a logistic mixer; and `mix9`, the default, which keeps a
bit history in each context, adds a match model and two word contexts, and
refines the mix with two adaptive probability maps.

And one for market data, stage 5's: `itch` parses the messages of a NASDAQ
TotalView-ITCH 5.0 feed, keeps the orders they describe, and codes each
field against what those orders say it should be, with the same coder and
mixer. It is experimental: measured on the first 30,000,000 messages of a
day so far, not yet on the whole day its brief asks for, and its archives
may change until it is tagged.

## Build

Requires CMake 3.21+ and a C++20 compiler (MSVC, GCC or Clang). No other
dependencies.

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows the binaries land in `build/Release/`; elsewhere in `build/`.
Keep the project in a short path on Windows: MSBuild fails when paths exceed
260 characters.

## Use

```
cpress c notes.txt            # writes notes.txt.cpz
cpress d notes.txt.cpz out.txt
cpress info notes.txt.cpz     # header: codec, sizes, checksum
cpress codecs
```

`cpress` never overwrites an existing file unless you pass `-f`.

`mix9` sizes its model to the input: about 4.3 MiB for a file of a kilobyte,
980 MiB for anything over 4 MiB. (`mix6` needs 190 KiB to 640 MiB.)
Decompression needs the same memory as compression did.

```
cpress c --codec itch 12302019.NASDAQ_ITCH50
```

`itch` is asked for by name. It reads and writes a block at a time, so the
file can be larger than memory: its model is 12.3 MiB to 364 MiB, and
decompression also holds the archive. Any file round-trips through it, but
only ITCH 5.0 messages, each preceded by its 2-byte length as in Nasdaq's
day files, compress well.

## Benchmark

```
cpress-bench corpus/enwik8
cpress-bench --cpu 2 --repeat 3 corpus/enwik8
```

Each file is compressed, decompressed and verified in its own process, then
one row is appended to `bench-results.csv`: git commit, codec, sizes, the
archive's CRC-32, bits per byte, nanoseconds per byte and MB/s each way, the
number of repeats, peak memory, and how much of each model was on large
pages. A file that is ITCH messages from end to end also gets its message
count and the archive's bytes per message, and a row for `itch` the number
of orders its table had to evict. See [corpus/README.md](corpus/README.md)
for where to get the input files.

`itch` is measured file to file, without holding the input, so a day file
of any size can be benchmarked; its archive goes to the temporary directory
and is removed. `bench/baselines.sh` runs the compressors it is compared
with, and `cpress-columns` is the splitter that script uses
([corpus/README.md](corpus/README.md)).

Timings vary by several percent from run to run. `--cpu N` pins the
measuring process to logical processor N, which keeps it off the slower
cores of a processor that has two kinds; `--repeat K` runs the round trip K
times and reports the fastest of each direction. Compare rows only with
rows from the same sitting. A `bench-results.csv` written by an older
version has other columns, and the benchmark will not append to it: move it
aside.

## Results

`cpress-bench corpus/enwik8` (100,000,000 bytes), Windows 11, MSVC Release,
single thread. Speed is recorded, not tuned; timings vary by about 10% from
run to run on this machine.

| Codec | Commit | Size (bytes) | Bits/byte | Compress ns/B | Decompress ns/B | Peak MiB |
|---|---|---|---|---|---|---|
| `mix9` | `v0.3` | 19,687,838 | 1.575 | 778.9 | 825.7 | 1194.2 |
| `mix9`, `kMaxLevel` 22 | experiment, not committed | 19,849,694 | 1.588 | – | – | – |
| `mix9`, no match model | experiment, not committed | 20,323,230 | 1.626 | – | – | – |
| `mix6` | `v0.2` | 22,527,170 | 1.802 | 381.5 | 402.5 | 856.8 |
| `mix6`, levels capped at 20 | experiment, not committed | 23,459,891 | 1.877 | – | – | – |
| `mix6`, one weight set | experiment, not committed | 22,864,484 | 1.829 | – | – | – |
| `order0` | `v0.1` | 61,263,585 | 4.901 | 31.0 | 45.5 | 254.4 |
| `order0`, adaptation shift 4 | experiment, not committed | 61,947,984 | 4.956 | – | – | – |

The shipped rows were measured at `9362c80`, `0eb8c7d` and `1aea9b1`, on
the `stage-3`, `stage-2` and `stage-1` branches, before each stage was
merged. Their model and coder code is identical to the tagged code, which
makes the same archives.

The `mix9` archive of `enwik8` has CRC-32 `6ee44655`, on MSVC and GCC
alike. Codec 3 is frozen, so every later commit must make exactly this
archive; `cpress-bench` prints the CRC-32 of each archive it makes. Every
stage 4 row below made it.

### Stage 4: the same archive, faster

`cpress-bench --cpu 2 --repeat 3 corpus/enwik8`: pinned to one logical
processor, the fastest of three runs, the builds alternated and each run
twice; the faster of the two is shown. i9-12900K. Linux: Ubuntu 24.04 under
WSL 2, GCC 13.3, transparent huge pages on `madvise`. Windows: Windows 11,
MSVC 19.51, with the "Lock pages in memory" privilege. Both put 980 MiB of
the model on large pages. `3aa6395` has v0.3's model and coder with stage
4's benchmark; `9d35533` is stage 4. Both are on the `stage-4` branch,
measured before the stage was merged; `v0.4` is that branch's last commit,
file for file.

| Build | Commit | Compress ns/B | Decompress ns/B | Change |
|---|---|---|---|---|
| Linux, v0.3 model | `3aa6395` | 695.8 | 727.0 | |
| Linux, stage 4 | `9d35533` | 472.6 | 599.1 | −32.1% / −17.6% |
| Windows, v0.3 model | `3aa6395` | 756.8 | 750.4 | |
| Windows, stage 4 | `9d35533` | 457.9 | 579.9 | −39.5% / −22.7% |

Linux from 15:08 to 15:34 UTC, Windows from 15:47 to 16:14, 12 November
2026. The stage's targets were 25% faster compressing and 10% faster
decompressing on Linux: **both are met.** `6eda504`, the commit after,
changes no model code that runs in a release build.

The first rows of this stage, at `c4d94df`, measured −26% / −7% on Linux.
Two things were wrong with them. GCC 13 inlined `Mix9::update()` into both
coding loops once they also called the hints, and the larger loops were
about 10% slower each way; `9d35533` keeps it out of line (stage 4 brief,
section 3). And the Windows rows were labelled "without large pages":
the account holds the privilege, `table_memory.cpp` enables it, and they
had them. The benchmark now reports how much of the model was on large
pages, so a row cannot be mislabelled that way again.

**Each technique alone,** `6eda504` with one line changed, not committed;
Linux, one sitting, 16:25 to 16:51 UTC, `--repeat 2`; the v0.3 row is the
faster of one run before the others and one after:

| Build | Compress ns/B | Decompress ns/B | Change |
|---|---|---|---|
| v0.3 model, `3aa6395` | 702.3 | 739.3 | |
| Large pages alone (no hints) | 623.6 | 667.7 | −11.2% / −9.7% |
| Hints alone (large pages refused) | 573.1 | 729.1 | −18.4% / −1.4% |
| Large pages and look-ahead, no guess | 463.8 | 661.5 | −34.0% / −10.5% |
| Stage 4, `6eda504` | 459.2 | 604.4 | −34.6% / −18.2% |

The encoder's look-ahead is the larger part of compression's gain. The
decoder's guess is worth 8.6% once the tables are on large pages, and
almost nothing without them: a prefetch whose address misses the
processor's page cache still waits for the page walk, and under WSL 2 that
walk crosses two page tables. That is an explanation, not a measurement.

**Where the time goes.** Levels 12 and 16 fit every history table in the
caches; they compress worse and make other archives, so they are a
measurement, never a configuration (stage 4 brief, section 4). Built with
`kMaxLevel` lowered; Linux, the sitting of the first table, level 23 at
`--repeat 3` and the others at `--repeat 2`:

| Level | Build | Size (bytes) | Compress ns/B | Decompress ns/B |
|---|---|---|---|---|
| 23 | v0.3 model | 19,687,838 | 695.8 | 727.0 |
| 23 | stage 4 | 19,687,838 | 472.6 | 599.1 |
| 16 | v0.3 model | 22,996,218 | 502.6 | 545.3 |
| 16 | stage 4 | 22,996,218 | 467.7 | 534.2 |
| 12 | v0.3 model | 30,228,770 | 464.6 | 507.0 |
| 12 | stage 4 | 30,228,770 | 453.4 | 487.4 |

At v0.3, compressing at level 23 took 231 ns/byte more than at level 12:
about a third of the time was waiting for memory. After stage 4 the gap is
19 ns, 4%; compression is bound by computation, as the brief expected.
Decompression's gap shrinks from 220 ns to 112: the decoder knows the next
byte only when the match model guesses it rightly, so a fifth of its time
is still waiting. Stage 4 is no slower than v0.3 where there is nothing to
wait for, so the hints cost less than they save even then.

`mix9` is the stage 3 codec; its target was 20 MB or better. The prototype
in its brief measured 19,687,808 bytes of ideal code length; the archive
is 30 bytes more, 25 of them the header, level byte and coder flush. It
runs at level 23 on this file, so 980.4 MiB of its peak memory is the
model. It takes about twice as long per byte as `mix6`, as the brief
expected.

The two `mix9` experiments are the ones the stage brief asked for, each
built once from `63af883` with one thing changed. `9362c80` then capped
order 2's table, which changes no prediction, so both results stand.

- `kMaxLevel` lowered from 23 to 22. The model shrinks to 524.4 MiB, about
  half, and the archive grows by 0.8%.
- No match model: its input held at 0, and the mixer's weight set chosen by
  the previous byte alone. The archive grows by 3.2%.

They were built with GCC 13 on Linux (WSL 2, same machine). That build
makes the same archive from the shipped codec as MSVC does, all 19,687,838
bytes of it, at 753 / 753 ns per byte; the experiments ran at 715 / 748
and 776 / 797.

`mix6` is the stage 2 codec, as shipped; its target was under 25 MB. It runs
at level 22 on this file, so 640.5 MiB of its peak memory is the model's
tables. The rest is the benchmark holding the input, the archive and the
restored copy at once.

The two `mix6` experiments are the alternatives the stage brief asked to be
measured. Each was built once from `0eb8c7d` with one thing changed:

- `kMaxLevel` lowered from 22 to 20. The tables shrink to 160.5 MiB, a
  quarter, and the archive grows by 4.1%.
- One weight set in the mixer, in place of 256 chosen by the previous byte.
  The archive grows by 1.5%.

They were built with GCC 13 on Linux (WSL 2, same machine), so their timings
are not in the table. That build makes the same archive from the shipped
codec as MSVC does, all 22,527,170 bytes of it, at 432 / 437 ns per byte;
the experiments ran at 379 / 392 and 427 / 443.

The `order0` rows are stage 1's: the codec as shipped, with an adaptation
shift of 5, and the alternative its brief asked to be measured. That one was
built once from `403ed8e` with the shift changed, and its timings matched
shift 5 within noise. It cannot be built from the current tree: the model
rejects a shift of 4 at compile time. `order0`'s model is 512 bytes, and its
target was 64 MB or better.

### Stage 5: market data, so far

The stage's benchmark is a whole day of the NASDAQ TotalView-ITCH 5.0 feed,
8.25 GB ([corpus/README.md](corpus/README.md)). These rows are on its first
30,000,000 messages, 951,200,923 bytes, cut with `cpress-columns --head`:
the codec is measured, the day is not yet, and neither are the second day
and the ablations that close the stage.

| Compressor | Size (bytes) | Bytes/message | Compress MB/s | Decompress MB/s |
|---|---|---|---|---|
| `cpress`, codec `itch` | 118,013,377 | 3.934 | 2.06 | 2.84 |
| columns, `zstd -19` each (the fair baseline) | 241,223,312 | 8.041 | 3.36 | – |
| `cpress`, codec `mix9` | 243,566,729 | 8.119 | 1.42 | 0.98 |
| `xz -9` | 275,980,244 | 9.199 | 1.01 | 43.51 |
| `zstd --ultra -22` | 304,533,474 | 10.151 | 0.92 | 576.84 |
| `zstd -19` | 306,219,590 | 10.207 | 1.80 | 655.55 |
| `gzip -9` | 385,060,713 | 12.835 | 1.92 | 115.89 |

`itch` is 51.1% smaller than the fair baseline, against the 45% the stage
asks for on the day, and 51.5% smaller than `mix9`. The fair baseline is
what a careful engineer gets without a model: every field of every message
type split into a column, each column compressed with `zstd -19` in its
best of three encodings (as it is, its differences, or its differences as
varints), and nothing charged for choosing. It is a size and a compression
speed, its time being `zstd`'s alone; nothing puts the columns back
together.

The `itch` row is `cpress-bench --codec itch --cpu 2`: Windows 11, MSVC
19.51, i9-12900K, one thread, file to file. The archive has CRC-32
`269d0221`. Its peak memory was 373 MiB compressing and 485 MiB
decompressing, which holds the archive; 360 MiB of it was the model, on
large pages. The order table, 2^23 entries at this size, evicted 160
orders. The prototype in the stage brief made 118,011,212 bytes of the same
messages with an unbounded table: the shipped codec is 2,165 bytes larger,
0.002%. The other rows are `bench/baselines.sh` on the same machine under
WSL 2, Ubuntu 24.04: gzip 1.12, zstd 1.5.5, xz 5.4.5, one core each; the
`mix9` row is GCC 13.3. The script took 2 hours 16 minutes.

## Archive format (version 1)

All fields are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Magic `CPRS` |
| 4 | 1 | Format version |
| 5 | 1 | Codec id |
| 6 | 2 | Reserved, must be zero |
| 8 | 8 | Original size in bytes |
| 16 | 4 | CRC-32 of the original data |
| 20 | … | Codec payload |

Decompression rejects an archive whose decoded size or checksum does not
match the header.

### Codecs

| Id | Name | Payload |
|---|---|---|
| 0 | `stored` | The original bytes |
| 1 | `order0` | Arithmetic-coded bits, then 4 flush bytes (frozen at `v0.1`) |
| 2 | `mix6` | One byte, the model's memory level; then arithmetic-coded bits and 4 flush bytes (frozen at `v0.2`) |
| 3 | `mix9` | As `mix6`, with a level of 10 to 23 (frozen at `v0.3`) |
| 4 | `itch` | One byte, the model's memory level, 10 to 23; then arithmetic-coded bits and 4 flush bytes (experimental until `v0.5`) |

`order0`, `mix6` and `mix9` code each byte most significant bit first. Every bit is
coded with a 12-bit probability P(bit = 1) from the model, by a carryless
32-bit binary arithmetic coder. The coded part of the payload is exactly the
coder's output; the decoder must consume it exactly, and the header's
original size may not exceed 4096 times its size.

The level byte of `mix6` is 10 to 22; any other value is rejected. Each of
the model's larger tables has 2^level slots of 32 bytes, which comes to
192 KiB of tables at level 10 and 640.5 MiB at level 22. The compressor
picks the smallest level with 2^level at least the input size. The
decompressor allocates what the archive's level calls for, so the level can
be chosen differently later without a format change.

The level byte of `mix9` is 10 to 23, chosen and checked by the same rules.
Each of its eight history tables has 2^level slots of 16 bytes, except that
orders 1 and 2 never have more than 2^14 and 2^22. Its match model has a
buffer of 2^level × 16 bytes. With fixed tables of 4.2 MiB that comes to
4.3 MiB at level 10 and 980.4 MiB at level 23.

`itch` does not code bytes. Its bits code a sequence of things, each either
an ITCH 5.0 message or one byte that is not part of a message, so that any
input round-trips. A message is recognised by its 2-byte big-endian length
being the length of the type that follows; the length is not coded, and the
fields are coded in an order of the model's choosing, as differences from
what the orders seen so far predict ([docs/stage-5.md](docs/stage-5.md)).
A message can cost fewer bits than it has bytes, so the header's original
size may be up to 33,792 times the payload's size. The level is the base-2
logarithm of the number of orders the model can hold at once: 12.3 MiB of
model at level 10, 364 MiB at 23, the smallest level whose table has an
entry for every 8 bytes of input.

## Layout

| Path | Contents |
|---|---|
| `include/cpress/` | Public API |
| `src/` | Container, codec registry, checksum, file I/O, streaming |
| `src/coder/` | Binary arithmetic coder |
| `src/model/` | The `Predictor` concept, the models and the parts they are built from |
| `src/itch/` | The ITCH 5.0 schema, the order table and market state, and the message model |
| `src/codec/` | The drivers that run a model over an input: bit by bit, and message by message |
| `src/cli/` | The `cpress` tool |
| `tests/` | Component, golden-archive, round-trip, corruption and mutation tests |
| `bench/` | The `cpress-bench` runner, the `cpress-columns` splitter and the baseline script |
| `corpus/` | Benchmark inputs (not tracked) |
| `docs/` | The idea, roadmap, stage briefs and code style |

## Design rules

The full conventions are in [docs/style.md](docs/style.md).

- One predictor, shared exactly by compressor and decompressor.
- Integer-only model arithmetic, so archives decode bit-identically on every
  compiler and platform.
- Models composed at compile time; no virtual calls in the per-bit loop.
- Once a stage is tagged, its codec ids and their output are permanent.

## Roadmap

| Stage | Goal | Target on enwik8 (100 MB) |
|---|---|---|
| 0 | Harness: container, CLI, tests, benchmark | done |
| 1 | Binary arithmetic coder + order-0 model | done: 61.3 MB (target 64 MB or better) |
| 2 | Hashed order 1–6 models + logistic mixer | done: 22.5 MB (target under 25 MB) |
| 3 | Bit-history states, probability maps, match model | done: 19.7 MB (target 20 MB or better) |
| 4 | Large pages, prefetching ahead; same archives | done: same archive, compress 32% faster, decompress 18% (target 25% and 10%) |
| 5 | Market data: an order-table model for exchange feeds | in progress: 3.93 bytes per message on the first 30M messages of a day, 51% under column-split + zstd (target on the whole day: 4.00 or better, and 45%) |
| 6 | Write-up and benchmark submission | |

## Documentation

| Document | Contents |
|---|---|
| [docs/idea.md](docs/idea.md) | Why the project exists, how context mixing works, goals and non-goals |
| [docs/roadmap.md](docs/roadmap.md) | Every stage in detail, with exit criteria |
| [docs/stage-1.md](docs/stage-1.md) | The brief for stage 1 |
| [docs/stage-2.md](docs/stage-2.md) | The brief for stage 2 |
| [docs/stage-3.md](docs/stage-3.md) | The brief for stage 3 |
| [docs/stage-4.md](docs/stage-4.md) | The brief for stage 4 |
| [docs/stage-5.md](docs/stage-5.md) | The brief for stage 5 |
| [docs/style.md](docs/style.md) | Code style and review checklist |
