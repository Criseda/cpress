# Roadmap

Seven stages. Each ends with something that works and a number to publish.
A stage is finished when its exit criteria are met, not when the ideas run
out. Time estimates assume part-time work and are guesses.

Sizes are on `corpus/enwik8` (100,000,000 bytes) unless stated.

| Stage | Goal | Exit number | Estimate |
|---|---|---|---|
| 0 | Harness | Round trip and benchmark work | done |
| 1 | Arithmetic coder + order-0 model | 64 MB or better (61.3 MB) | done |
| 2 | Context mixing | under 25 MB (22.5 MB) | done |
| 3 | lpaq-class model | 20 MB or better | 3–4 weeks |
| 4 | Performance | same size at 870 ns/byte or better | 3–4 weeks |
| 5 | Market data | beat column-split + zstd | 6–8 weeks |
| 6 | Release | write-up published | 2 weeks |

## How stages run

1. A brief is written for the stage (`docs/stage-N.md`): components,
   interfaces, invariants, tests, exit criteria, and what is out of scope.
2. The stage is implemented against the brief in small commits.
3. The stage is reviewed against the checklist in `docs/style.md`.
4. Results go into the README, the stage is tagged (`v0.N`), and the codec
   ids it introduced are frozen.

Until a stage is tagged, its codec is experimental and its output may change.
After the tag, changed predictions mean a new codec id.

---

## Stage 0: harness (done)

Container format, `cpress` CLI, round-trip and corruption tests, and
`cpress-bench`, which records size, speed, peak memory and git commit per
file. The only codec is `stored`.

## Stage 1: arithmetic coder and order-0 model (done)

Brief: [stage-1.md](stage-1.md).

- A binary arithmetic coder driven by a 12-bit probability.
- The `Predictor` concept: the compile-time interface every model implements.
- A generic bitwise codec that runs any `Predictor` in both directions.
- An order-0 model: one adaptive probability per partial-byte context.

**Exit:** 64 MB or better; identical archive bytes on MSVC, GCC and Clang.

## Stage 2: context mixing (done)

Brief: [stage-2.md](stage-2.md).

- **Hashed context models** for orders 1 to 6: the last *n* bytes are hashed
  to a slot holding adaptive probabilities for the bits of the next byte.
- **Logistic mixing:** predictions are combined in the stretched domain,
  `stretch(p) = ln(p / (1 - p))`, by a weighted sum whose weights are trained
  online to reduce coding cost. Weight sets are selected by a small context.
- **Fixed-point `stretch` and `squash` tables**, generated at compile time.
- A memory level, stored in the archive, so table sizes are explicit.

**Exit:** under 25 MB, which beats xz and zstd. Speed is recorded, not tuned.

The brief settles what this section left open, by measurement: a Fibonacci
hash to a 32-byte slot of sixteen counters per nibble; 256 weight sets,
chosen by the previous byte; a fixed learning rate.

## Stage 3: lpaq-class model

- **Bit-history states:** each context slot holds a small state summarising
  the recent bits seen there, instead of a single probability. The state
  table is designed here and generated at compile time, not copied.
- **Adaptive state maps:** state to probability, learned online.
- **Secondary estimation:** one or two stages that refine the mixer's output
  given a small context, interpolating between buckets.
- **Match model:** finds the longest recent repeat of the current context and
  predicts the bit that followed it, with confidence from the match length.
- A word-level context for text.

**Exit:** 20 MB or better. Stretch: 18.0 MB, the size lpaq9m reaches.

**Open questions for the brief:** state design (counts, recency, how states
age); how collisions are detected and slots replaced.

## Stage 4: performance

No change to compressed size beyond 0.1%. The work is making the same model
fast.

- **Cache-line buckets:** one 64-byte bucket serves a whole half-byte of
  predictions, so a model costs one cache miss per nibble instead of one per
  bit. Buckets carry a checksum for collision detection.
- **Prefetching** the next buckets at each nibble boundary.
- **SIMD mixer:** the dot product and weight update in AVX2, with a scalar
  path that produces identical results. Integer SIMD only.
- Measurement with hardware counters: cache misses per byte, branch misses.
- The ratio-against-speed plot and the script that draws it.

**Exit:** 870 ns/byte or better (lpaq9m's benchmark figure) at stage 3's
size. Stretch: 500 ns/byte.

## Stage 5: market data

Three parts, each useful alone.

**5a. Parser and baselines.**
- An ITCH 5.0 message parser and per-message-type statistics.
- Baselines on the same file: gzip -9, zstd -19, xz -9, cpress's text model,
  and the fair baseline: split each field into its own column, delta-encode
  where sensible, compress each column with zstd.

**5b. Structured model.**
- Contexts keyed by message type and field position.
- Timestamps and order reference numbers coded as deltas.
- An order table: a cancel or execution is predicted from the order it
  refers to.
- A per-stock book: prices are coded relative to the best bid and ask.

**5c. Streaming.**
- Day files reach about 13 GB, so the whole-file-in-memory I/O from stage 0
  is replaced by streaming for this codec.

**Exit:** a measured, reproducible comparison against every baseline in
bytes per message and MB/s. If the structured model beats the fair baseline
by less than 10% while being far slower, that is the result and the write-up
reports it as such.

**Data:** NASDAQ publishes sample day files; see `corpus/README.md`. Start
with the 12,012-message test file, then slices of a full day.

## Stage 6: release

- The write-up: design, the plot, an ablation table showing what each
  component is worth, and the market-data result.
- Submission to the Large Text Compression Benchmark (`enwik9`; the
  decompressor's size counts towards the total).
- Golden archives for every frozen codec.
- A licence, chosen before the repository is made public.
- Tag `v1.0`.

## After 1.0

Only if the classical design is finished and written up:

- A recurrent-network mixer, which is how cmix reaches the top of the table
  at several hundred times the running time.
- Dictionary preprocessing for text.
- Decoder fuzzing as a permanent CI job.

## Standing engineering work

These are not stages; they are added when first useful.

| Item | When |
|---|---|
| Decoder fuzzing | Stage 1, once a real decoder exists |
| Golden archives | When the first codec id is frozen |
| Format check in CI | When a clang-format version is pinned |
| Cross-compiler archive comparison | Stage 1 (expected checksums in tests) |
