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
| 3 | lpaq-class model | 20 MB or better (19.7 MB) | done |
| 4 | Performance | same archive; compress 25%, decompress 10% faster (32%, 18%) | done |
| 5 | Market data | 4.00 bytes per message or better; 45% under column-split + zstd (3.88; 52%) | done |
| 6 | Release | write-up published ([write-up.md](write-up.md)) | done |

## How stages run

1. A brief is written for the stage (`docs/stages/stage-N.md`): components,
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

Brief: [stage-1.md](stages/stage-1.md).

- A binary arithmetic coder driven by a 12-bit probability.
- The `Predictor` concept: the compile-time interface every model implements.
- A generic bitwise codec that runs any `Predictor` in both directions.
- An order-0 model: one adaptive probability per partial-byte context.

**Exit:** 64 MB or better; identical archive bytes on MSVC, GCC and Clang.

## Stage 2: context mixing (done)

Brief: [stage-2.md](stages/stage-2.md).

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

## Stage 3: lpaq-class model (done)

Brief: [stage-3.md](stages/stage-3.md).

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

The brief settles what this section left open, by measurement: a state is a
pair of counts, with the opposite count halved when the bit changes and no
record of recency; a slot carries an 8-bit check, four slots share a 64-byte
bucket, and the least-trained is replaced. Word contexts are two: the
current word, and it with the word before. A prototype of the design
measured 19.69 MB.

## Stage 4: performance (done)

Brief: [stage-4.md](stages/stage-4.md).

The same model, faster, with codec 3's archives unchanged byte for byte.

- **Large pages** for the model's tables, where the operating system grants
  them without new privileges.
- **Prefetching the next byte's slots:** from the input when compressing,
  which knows it, and from the match model's guess when decompressing.
- A benchmark that can see a 5% change: pinned, repeated, minimum taken.
- The memory share of the time, measured without hardware counters.

**Exit:** on `enwik8`, compression 25% and decompression 10% faster than
`v0.3`, with an identical archive. Measured: 32% and 18% on Linux, 40% and
23% on Windows; the README has the rows.

The brief settles what this section first planned, by measurement: `v0.3`
already met the old exit, 870 ns/byte. Aligning buckets and prefetching at
the nibble boundary gained nothing, and a SIMD mixer could gain little.
The computation, threads and the old stretch figure, 500 ns/byte, are
recorded in issue #4 for after 1.0; the ratio-against-speed plot moves to
stage 6.

## Stage 5: market data (done)

Brief: [stage-5.md](stages/stage-5.md).

One day of the NASDAQ TotalView-ITCH 5.0 feed: 268,744,780 messages,
8.25 GB. Three parts, each useful alone.

**5a. Schema, benchmark and baselines.**
- The ITCH 5.0 message schema as a table, and per-message-type statistics.
- Baselines on the same file: gzip -9, zstd -19, xz -9, cpress's text model,
  and the fair baseline: split each field into its own column, delta-encode
  where sensible, compress each column with zstd.

**5b. Structured model.**
- Each field coded as its difference from what the messages so far lead
  the model to expect, through the history tables, mixer and APM of stages
  2 and 3, in contexts keyed by message type and field.
- Timestamps, order reference numbers and match numbers coded as
  differences.
- An order table: a cancel, execution or replace names its order by its
  place among the stock's live orders, and is predicted from the order.
- The stock itself coded by how recently it was last seen.

**5c. Streaming.**
- The day file does not fit the whole-file-in-memory I/O from stage 0, so
  codec 4 compresses and decompresses file to file.

**Exit:** on the day file, 4.00 bytes per message or better, and at least
45% smaller than the fair baseline, in a measured, reproducible comparison
against every baseline in bytes per message and MB/s; and the same sizes
on a second day. Measured: 3.884 bytes per message and 51.7% under the
fair baseline on the day; 3.843 and 51.4% on the second day, which was not
used in design. The README has the rows.

**Data:** NASDAQ publishes sample day files; see `corpus/README.md`.
Development uses the first 30,000,000 messages of the day; the exit is on
all of it.

The brief settles what this section left open, by measurement. The fair
baseline is 8.04 bytes per message and xz 9.26; a prototype of the design
made 3.88 and decoded the whole day back. Differences are worth 35% of
the model's size and the order table 14% more. The per-stock book this
section planned, for coding prices against the best bid and ask, is worth
0.5% and is left out. The codec decompresses at about 4 MB/s, a small
fraction of any baseline's speed; that is part of the result. The
12,012-message test file this section once named is not in NASDAQ's
listing. Blocks and threads, which issue #4 left to this stage, stay out:
an order message depends on an order added hours before.

## Stage 6: release (done)

Brief: [stage-6.md](stages/stage-6.md).

- The write-up: design, the ratio-against-speed plot and the script that
  draws it, an ablation table showing what each component is worth, and the
  market-data result.
- `enwik9` measured for the Large Text Compression Benchmark, with the
  decompressor's size, which counts towards the total. The submission is
  drafted, and sent after 1.0, when the owner is content with cpress's
  performance.
- Golden archives for every frozen codec.
- The licence, Apache-2.0, added before the repository is made public.
- Tag `v1.0`.

## After 1.0

Only if the classical design is finished and written up:

- A recurrent-network mixer, which is how cmix reaches the top of the table
  at several hundred times the running time.
- Dictionary preprocessing for text.
- Speed beyond stage 4: the computation, a block-parallel codec, and the
  rest measured in issue #4.
- Decoder fuzzing as a permanent CI job.

Whatever comes of these, the submission to the Large Text Compression
Benchmark that stage 6 drafted is sent when the owner is content with
cpress's performance on `enwik9`.

## Standing engineering work

These are not stages; they are added when first useful.

| Item | When |
|---|---|
| Decoder fuzzing | Stage 1, once a real decoder exists |
| Golden archives | When the first codec id is frozen |
| Format check in CI | When a clang-format version is pinned |
| Cross-compiler archive comparison | Stage 1 (expected checksums in tests) |
