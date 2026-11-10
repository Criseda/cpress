# cpress

A lossless compressor built on context mixing, written from scratch in C++20.

**Status: stage 0.** The container format, command-line tool, tests and
benchmark runner work end to end. The only codec is `stored`, which does not
compress; it exists so everything around the model is proven first.

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

## Benchmark

```
cpress-bench corpus/enwik8
```

Each file is compressed, decompressed and verified in its own process, then
one row is appended to `bench-results.csv`: git commit, codec, sizes, bits
per byte, nanoseconds per byte each way, and peak memory. See
[corpus/README.md](corpus/README.md) for where to get the input files.

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

## Layout

| Path | Contents |
|---|---|
| `include/cpress/` | Public API |
| `src/` | Container, codec registry, checksum, file I/O |
| `src/cli/` | The `cpress` tool |
| `tests/` | Round-trip and corruption tests |
| `bench/` | The `cpress-bench` runner |
| `corpus/` | Benchmark inputs (not tracked) |
| `docs/` | Code style and review checklist |

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
| 1 | Binary arithmetic coder + order-0 model | 64 MB or better |
| 2 | Hashed order 1–6 models + logistic mixer | under 25 MB |
| 3 | Bit-history states, probability maps, match model | 20 MB or better |
| 4 | Cache-aware tables, prefetching, SIMD mixer | same size at 870 ns/byte or better |
| 5 | Market data: order-book-aware model for exchange feeds | beat column-split + zstd |
| 6 | Write-up and benchmark submission | |

## Documentation

| Document | Contents |
|---|---|
| [docs/idea.md](docs/idea.md) | Why the project exists, how context mixing works, goals and non-goals |
| [docs/roadmap.md](docs/roadmap.md) | Every stage in detail, with exit criteria |
| [docs/stage-1.md](docs/stage-1.md) | The brief for the current stage |
| [docs/style.md](docs/style.md) | Code style and review checklist |
