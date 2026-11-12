# cpress

A lossless compressor built on context mixing, written from scratch in C++20.

**Status: stage 3 done, tagged `v0.3`; stage 4 is next.**
The container format, command-line tool, tests and benchmark runner work end
to end. There are four codecs: `stored`, which does not compress; `order0`, a
binary arithmetic coder driven by an order-0 bitwise model; `mix6`, which
drives the same coder with six hashed context models, orders 1 to 6,
combined by a logistic mixer; and `mix9`, the default, which keeps a bit
history in each context, adds a match model and two word contexts, and
refines the mix with two adaptive probability maps.

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

## Benchmark

```
cpress-bench corpus/enwik8
cpress-bench --cpu 2 --repeat 3 corpus/enwik8
```

Each file is compressed, decompressed and verified in its own process, then
one row is appended to `bench-results.csv`: git commit, codec, sizes, the
archive's CRC-32, bits per byte, nanoseconds per byte each way, the number
of repeats, and peak memory. See [corpus/README.md](corpus/README.md) for
where to get the input files.

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
archive; `cpress-bench` prints the CRC-32 of each archive it makes.

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

## Layout

| Path | Contents |
|---|---|
| `include/cpress/` | Public API |
| `src/` | Container, codec registry, checksum, file I/O |
| `src/coder/` | Binary arithmetic coder |
| `src/model/` | The `Predictor` concept, the models and the parts they are built from |
| `src/codec/` | The generic bitwise encode/decode driver |
| `src/cli/` | The `cpress` tool |
| `tests/` | Component, golden-archive, round-trip, corruption and mutation tests |
| `bench/` | The `cpress-bench` runner |
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
| 4 | Large pages, prefetching ahead; same archives | compress 25%, decompress 10% faster |
| 5 | Market data: order-book-aware model for exchange feeds | beat column-split + zstd |
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
| [docs/style.md](docs/style.md) | Code style and review checklist |
