# The idea

cpress is a lossless compressor built on context mixing, written from scratch
in C++20 with no dependencies. It has two targets: general text, where a
public benchmark exists, and exchange market data, where no specialised
public compressor does.

## Why compression

Compression is prediction. An arithmetic coder turns a probability for the
next bit into output whose length is exactly the log-loss of that probability.
A better predictor is a smaller file, bit for bit. That makes a compressor an
online machine-learning system running in a very tight loop, and it gives the
project properties that are rare in one place:

- **The metric has no variance.** The score is a byte count. The same input
  always gives the same number, immediately, as often as you like.
- **Correctness is checkable.** Decompression must return the input exactly.
  A wrong model cannot hide; it fails the round trip.
- **It needs real systems work.** Each predicted bit touches several tables
  far larger than the cache, so memory layout decides the speed.
- **It works at every stage.** There is a usable compressor from stage 1, and
  every later stage only makes the number smaller or the tool faster.
- **Prediction is betting.** Code length is the quantity a Kelly bettor
  maximises. Compressing a market feed well means modelling the market's
  microstructure well.

## How context mixing works

```
            history of bits seen so far
                       |
     +--------+--------+--------+--------+
     |        |        |        |        |
  order-1  order-2   ...     match    (many small models, each
  model    model            model      predicts the next bit)
     |        |        |        |
     +--------+---+----+--------+
                  |
               mixer            (a tiny network, trained online,
                  |              weighs the models per context)
          probability map       (final adjustment of the estimate)
                  |
          arithmetic coder  ->  compressed bits
```

Every bit is predicted, coded, and then used to update every model and the
mixer. The decompressor runs the identical predictor, so it sees the same
probabilities and can reverse the coder. Nothing about the model is stored in
the archive; both sides rebuild it as they go.

## Two targets

**General text.** The Large Text Compression Benchmark ranks programs on a
fixed 1 GB Wikipedia extract (`enwik9`); its first 100 MB (`enwik8`) is the
development file. This gives every stage a public number to beat.

**Market data.** A full day of the NASDAQ ITCH feed is hundreds of millions of
small binary messages: orders added, cancelled, executed. To predict the next
message well, the model has to track the order book. Vendors compress these
files with general-purpose tools; a model that understands the structure
should do much better. That is the experiment, not a known result.

## Reference numbers

Sizes on `enwik8`, from the Large Text Compression Benchmark. Its times
are on `enwik9`, on whichever machine each program was tested on (a row
without a note, gzip's among them, on an Athlon 64 3500+ under 32-bit
Windows XP), so they rank programs only roughly. Stage 6 measured the programs that run in minutes
on one machine; [the write-up](write-up.md#7-where-cpress-sits) has
those numbers.

| Program | Size (MB) | Compression speed (ns/byte) | Kind |
|---|---|---|---|
| gzip -9 | 36.4 | 101 | LZ77 |
| zstd -22 | 25.4 | 701 | LZ77 + entropy coding |
| xz -9e | 24.7 | 5,876 | LZMA |
| 7zip PPMd | 21.2 | 503 | PPM |
| bsc | 20.8 | 23 | block sorting |
| lpaq9m | 18.0 | 868 | small context mixer |
| paq8px | 15.8 | 291,916 | large context mixer |
| cmix v21 | 14.6 | 622,949 | context mixing + neural networks |

The realistic territory for cpress is the lpaq class: about 18 to 20 MB at
around a microsecond per byte.

## What success looks like

1. A ratio-against-speed plot placing cpress beside the programs above.
2. A measured result on market data against zstd and against the fair
   baseline (fields split into columns, delta-encoded, then zstd).
3. A write-up explaining what each component contributes, with an ablation
   table.
4. Code that a stranger can read top to bottom and trust.

## Non-goals

- **Topping the text benchmark.** The leaders are years of tuning on one file
  and run for days.
- **Replacing 7-Zip.** No archive of many files, no window, no shell menu.
- **Fast decompression.** Context mixing is nearly symmetric: the decoder
  runs the same model, bit for bit, so it does the same work. The one
  difference is in the encoder's favour. It has the whole input, so it can
  fetch the memory the next byte will touch before the model asks for it;
  the decoder can only guess the next byte (stage 4). Decoding costs at
  least as much as encoding. This is an archival tool.
- **Neural mixers before the classical design is finished.** They come after
  stage 5, if at all.

## Honest risks

- **The market-data margin.** Column splitting plus zstd might have got
  most of the gain. Measured in stage 5, it did not: on a whole day of the
  feed cpress is 52% smaller than the columns, and 51% on a second day
  not used in design.
- **Silent non-determinism.** One floating-point operation or one piece of
  undefined behaviour in the model makes archives that only decode on the
  machine that wrote them. The style rules exist largely to prevent this.
- **Scope.** Each stage has an exit number. Reaching it ends the stage.

## Reading

- Matt Mahoney, *Data Compression Explained*: <https://mattmahoney.net/dc/dce.html>
- Large Text Compression Benchmark: <https://mattmahoney.net/dc/text.html>
- Matt Mahoney, *Adaptive Weighing of Context Models for Lossless Data
  Compression* (2005), the PAQ paper.
- Witten, Neal and Cleary, *Arithmetic Coding for Data Compression* (1987).
- cmix, by Byron Knoll: <https://www.byronknoll.com/cmix.html>
- Hutter Prize: <http://prize.hutter1.net/>
- NASDAQ TotalView-ITCH 5.0 specification:
  <https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf>

Existing compressors are for reading and for comparison. No code or tables are
copied from them: several are GPL-licensed, and the point of the project is
that it is written from scratch.
