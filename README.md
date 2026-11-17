# cpress

> The key to compression is to understand the data you want to compress.
>
> Matt Mahoney, [*Data Compression Explained*](https://mattmahoney.net/dc/dce.html#Section_15), section 1.5

cpress is a lossless compressor built on context mixing, written from
scratch in C++20 with no dependencies. Before each bit of its input it
asks many small models what comes next, mixes their answers into one
probability, and an arithmetic coder charges the bit what that probability
says it is worth: almost nothing when the prediction was confident and
right.

The same engine compresses two kinds of data. Given English text, it is a
general model in the class of Matt Mahoney's lpaq1: 19.7 MB of `enwik8`,
the first 100 MB of Wikipedia, a little smaller than lpaq1 itself. Given a
day of NASDAQ's ITCH market feed, it is a model that knows what an order
is. It keeps every live order the messages describe, names an order by its
place among its stock's recent orders instead of by its 64-bit number, and
codes each field as its difference from what the order says it should be.
That is 3.88 bytes a message: less than half of what a careful engineer
gets with every field in a column of its own and zstd.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/figures/understanding-dark.svg">
  <img alt="Bytes per ITCH message: cpress itch 3.93; the fair baseline 8.04; cpress mix9 8.12; xz 9.20; zstd 10.15 and 10.21; gzip 12.84." src="docs/figures/understanding-light.svg">
</picture>

*The first 30 million messages of 30 December 2019. `mix9` and `itch`
share the coder and the mixer; only `itch` understands the data.*

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/figures/ratio-speed-dark.svg">
  <img alt="Compressed size of enwik8 against nanoseconds per byte, compressing and decompressing, for cpress and other compressors." src="docs/figures/ratio-speed-light.svg">
</picture>

*Size against speed on `enwik8`. Filled points were measured on one
machine in one sitting; hollow ones are the Large Text Compression
Benchmark's own, timed on other machines.*

| Input | cpress | For comparison | Record |
|---|---|---|---|
| `enwik8`, the first 100 MB of English Wikipedia | **19,687,838 bytes**, 1.575 bits a byte (`mix9`) | lpaq1: 19,755,948 bytes, and slower on the same machine | [stage 6](docs/write-up.md#stage-6-the-text-compressors-on-enwik8) |
| A NASDAQ ITCH day, 8.25 GB, 268,744,780 messages | **3.884 bytes a message** (`itch`) | Every field a column, each `zstd -19`: 8.036. `xz -9`: 9.261 | [stage 5](docs/write-up.md#stage-5-market-data) |
| Another day, not used in design | **3.843 bytes a message** | The columns: 7.901 | [stage 5](docs/write-up.md#stage-5-market-data) |

**[How cpress works](docs/write-up.md):** the design from the coder up,
what each part is worth, where cpress sits among other compressors, and
every measurement with the commit that made it.

## Five codecs

- `stored` does not compress.
- `order0`: the arithmetic coder and an order-0 model, stage 1.
- `mix6`: six hashed context models, orders 1 to 6, combined by a logistic
  mixer, stage 2.
- `mix9`, the default: a bit history in each context, a match model, two
  word contexts, and two adaptive probability maps refining the mix. Stage
  3, made faster in stage 4 without changing a byte of its output.
- `itch`: the messages of a NASDAQ TotalView-ITCH 5.0 feed, coded against
  the orders they describe, with the same coder and mixer. Stage 5; asked
  for by name.

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

`cpress` never replaces an existing file unless you pass `-f`. The output is
written under a temporary name and renamed into place once it is complete,
so a failed run leaves no partial file and any old one as it was. `-f`
replaces the file rather than writing into it: a symbolic link at the
output's name becomes a regular file, and the new file has default
permissions.

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
| 4 | `itch` | One byte, the model's memory level, 10 to 23; then arithmetic-coded bits and 4 flush bytes (frozen at `v0.5`) |

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
| 5 | Market data: an order-table model for exchange feeds | done, on an ITCH day file: 3.88 bytes per message, 52% under column-split + zstd (target 4.00 or better, and 45%) |
| 6 | Write-up and benchmark submission | in progress: [docs/stage-6.md](docs/stage-6.md) |

## Documentation

| Document | Contents |
|---|---|
| [docs/write-up.md](docs/write-up.md) | How cpress works, what each part is worth, and every measurement |
| [docs/idea.md](docs/idea.md) | Why the project exists, how context mixing works, goals and non-goals |
| [docs/roadmap.md](docs/roadmap.md) | Every stage in detail, with exit criteria |
| [docs/stage-1.md](docs/stage-1.md) | The brief for stage 1 |
| [docs/stage-2.md](docs/stage-2.md) | The brief for stage 2 |
| [docs/stage-3.md](docs/stage-3.md) | The brief for stage 3 |
| [docs/stage-4.md](docs/stage-4.md) | The brief for stage 4 |
| [docs/stage-5.md](docs/stage-5.md) | The brief for stage 5 |
| [docs/stage-6.md](docs/stage-6.md) | The brief for stage 6 |
| [docs/style.md](docs/style.md) | Code style and review checklist |
