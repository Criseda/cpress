# Stage 6 brief: release

> The key to compression is to understand the data you want to compress.
>
> Matt Mahoney, *Data Compression Explained*, section 1.5

Stages 1 to 5 built two things with one engine: a compressor for text in
the lpaq class, and a compressor for one day of an exchange feed that
codes 52% smaller than the best a careful engineer gets without a model.
The second is the sentence above, measured. On the same 951 MB of ITCH
messages, `mix9`, which sees bytes, makes 8.12 bytes a message; `itch`,
the same coder and mixer pointed at the orders the messages describe,
makes 3.93.

Stage 6 writes that down, puts it where people will read it, and releases
the project. It adds no codec and changes no prediction. Almost every
number it needs already exists, in the README and the stage briefs; what
is new is a measurement of the text compressors on this machine, the
figures, the prose, a golden archive for codec 4, `enwik9`, a licence, and
the tag.

**Exit:** the write-up is published: `docs/write-up.md` and a README that
leads with it are on `main`, the repository is public under a licence,
`v1.0` is tagged, and the results have been sent to the Large Text
Compression Benchmark. The listing itself is Matt Mahoney's to make and is
not waited for.

*Amended in implementation:* the results are not sent in this stage.
cpress is still being developed, and the owner will send them when
content with its performance; the stage leaves the email drafted
(section 6). The rest of the exit stands.

## Scope

Four parts, in this order. Each is useful alone and may be its own pull
request.

**6a. Codec 4's golden archive.**

1. A few ITCH messages and one stray byte, their `itch` archive, both
   checked in as bytes in `tests/golden_test.cpp`.

**6b. Measurements and figures.**

2. The text compressors on `enwik8`, on this machine, in one sitting,
   with `cpress`'s codecs beside them: `bench/text-baselines.sh`.
3. The figures, drawn from checked-in tables by a script that uses only
   Python's standard library and writes SVG: `docs/figures/`.

**6c. The write-up and the README.**

4. `docs/write-up.md`: the design, the figures, the ablations, the
   market-data result, the measurements behind every number, and the
   sources.
5. The README rewritten to lead with the quote, the two main figures and
   a link to the write-up. Its detailed results tables move to the
   write-up, every number and commit hash with them.

**6d. Release.**

6. `enwik9`: sizes and times, and the decompressor's size as the
   benchmark counts it.
7. The licence: Apache-2.0.
8. The repository made public, `v1.0` tagged, and the results emailed to
   the benchmark. *Amended:* the email is drafted, not sent (the exit).

Out of scope:

- Any change to codecs 1 to 4, and any new codec. The additions that
  stage 3 measured and left ([stage 3](stage-3.md), "Measured, worth
  having") and stage 5's price levels are each a new codec id, after 1.0.
- Tuning anything for `enwik9`. The benchmark allows it; the project is
  not about the table.
- Running paq8px, cmix or nncp here. On the benchmark's own figures,
  paq8px takes about eight hours a direction on `enwik8` and cmix about
  seventeen; their points come from the benchmark (section 2).
- The Hutter Prize, whose rules are not the benchmark's.
- Binaries, installers and release automation. The release is the tagged
  source; the benchmark accepts source (section 6).
- An external post. The write-up is written so that one could be cut
  from it later.
- Everything in the roadmap's "After 1.0".

## Files

```
tests/golden_test.cpp        codec 4's golden archive beside codecs 1 to 3
bench/text-baselines.sh      the text compressors on one file, pinned, round trip checked
docs/figures/*.csv           the numbers each figure is drawn from, with their sources
docs/figures/draw.py         reads the tables, writes every figure, light and dark
docs/figures/*.svg           the figures, checked in
docs/write-up.md             the write-up
README.md                    rewritten to lead with the quote and the figures
LICENSE                      Apache-2.0
corpus/README.md             enwik9's checksum; how to run the text baselines
docs/roadmap.md, docs/idea.md, CLAUDE.md
```

## The choices

Each has a real trade-off; the recommendation is what the rest of the
brief assumes.

**Where the plot's points come from.** The Large Text Compression
Benchmark has a size and a time for nearly every compressor, and its sizes
are facts. Its times are not comparable with each other or with ours. They
are measured on `enwik9`, "not all tests are done on the same computer",
and a row with no note was timed on an Athlon 64 3500+ under 32-bit
Windows XP. The rows for the common tools are also old versions: zstd
0.6.0, xz 5.2.1, gzip 1.3.5, brotli of 2016. **Recommendation:** measure
every program that finishes `enwik8` in under an hour a direction here,
in one sitting with `cpress`, and take only the three slow neural and
context-mixing programs from the benchmark, drawn hollow and labelled as
timed on other machines. A log axis spanning five orders of magnitude
survives a factor of two or three; the caption still says it is there.

**What draws the figures.** The roadmap asks for the plot and the script
that draws it. matplotlib is the usual tool and draws well, but it is a
dependency the project otherwise does not have, and its SVG output
changes between versions. **Recommendation:** a Python script that uses
only the standard library and writes SVG by hand: a few hundred lines,
runs anywhere Python 3 does, and gives the same bytes from the same
tables, so a changed figure shows up in a diff as a changed number. The
alternative in keeping with the rest of the project, a C++ tool in
`bench/`, would be longer and harder to restyle.

**What the README keeps.** The README is now mostly measurement records:
sittings, commit hashes, explanations of rows that were replaced. They are
the project's evidence and must stay, but they are not what a visitor
should meet first. **Recommendation:** the README becomes the short,
readable front page; the records move, unchanged, to an appendix of the
write-up, and the README links to them.

**Whose voice.** The briefs and the README are impersonal, and so is
*Data Compression Explained*. *Decided:* the write-up is impersonal and
plain, and says nothing about its author.

**The licence** was the owner's call: Apache-2.0 (section 7).

## 1. Codec 4's golden archive

`tests/golden_test.cpp` has an archive for codecs 1 to 3. Codec 4 was
frozen at `v0.5` without one; the rule in [style.md](../style.md), Tests, is
that it gets one once it has shipped.

- **The input** is binary, so it is checked in as a byte array, not a
  string. About ten framed messages, enough that the order table changes
  in every way it can: at least an add, an execution, a cancel, a replace
  and a delete of an order added earlier in the input. After them, one
  byte that is not a message, so that the archive also fixes the escape.
  Generate it once from `itch_sample()` with a fixed seed, cut it to the
  messages needed, and paste the bytes; the comment records the seed and
  the count. The test must not call `itch_sample()`: a later change to the
  sample would then move the golden input.
- **The archive** is made by `cpress c --codec itch` from the code tagged
  `v0.5`, and the comment says so, as the others do. Its level byte is 10.
- **The check** is the one the others use: the archive decodes to the
  input, and the input compresses to exactly the archive. The helper
  takes a byte span in place of a `string_view`. `stream_test.cpp`
  already checks that file to file and buffer to buffer make the same
  archive for every codec, so the buffer path covers both.

The archive's size is whatever `v0.5` makes of it and measures nothing:
the length prefixes cost no bits, but ten messages teach the model little,
and the header and flush are 25 bytes.

## 2. The text baselines

**The claim:** where `cpress` sits among text compressors by size and by
speed in each direction, on one machine. **The input:** `enwik8`. A size is
a fact of the program and the file; a speed needs minutes of steady work,
which `enwik8` gives every program here. `enwik9` would add nothing to
either but hours.

**The programs,** at the benchmark's own options where it gives them, so
that each size can be checked against its row:

| Program | Options | Source |
|---|---|---|
| gzip | `-9` | Ubuntu 24.04 |
| bzip2 | `-9` | Ubuntu 24.04 |
| zstd | `--ultra -22` | Ubuntu 24.04 |
| xz | `--lzma2=preset=9e,dict=1GiB,lc=4,pb=0` | Ubuntu 24.04 |
| brotli | `-q 11 -w 24` | Ubuntu 24.04 |
| 7-Zip, PPMd | `-m0=PPMd:mem=1630m:o=10` | Ubuntu 24.04 (`7zip`, whose command is `7z`) |
| bsc | `-b1000 -e2`, one thread | built from source, Apache-2.0 |
| lpaq1 | `9` | built from source, GPL, <https://mattmahoney.net/dc/lpaq1.zip> |
| `cpress` | `order0`, `mix6`, `mix9` | this repository, GCC 13.3 |

bzip2, brotli and 7-Zip are not installed in the WSL image yet; bsc and
lpaq1 are built in a scratch directory and never enter the repository.
lpaq1 is the direct ancestor of `mix9` (the credits in `src/model/` say
where) and the one comparison a reader will most want on the same
machine: on the benchmark it makes 19,755,948 bytes of `enwik8` at option
9, and `mix9` makes 19,687,838.

**How each is measured** (`bench/text-baselines.sh`): pinned to one
logical processor with `taskset`, one program at a time on a quiet
machine, compress and decompress each three times, the fastest of each
taken, the output compared with the input byte for byte. The script
prints one CSV row per program with its version string, options, size,
both speeds in ns/byte and peak memory. The `cpress` rows are
`cpress-bench --cpu 2 --repeat 3` in the same sitting, and their sizes
must be the README's to the byte.

Expect about an hour. The slow rows are lpaq1, around a microsecond and a
half a byte each way, and brotli at quality 11. *Measured:* 35 minutes, 10
for `cpress`'s three codecs and 25 for the eight others; lpaq1 ran at 0.7
µs a byte each way, brotli at 1.5 compressing.

**From the benchmark,** sizes on `enwik8` and times as published, both
copied into the table with the benchmark's update date:

| Program | `enwik8` (bytes) | Compress ns/B | Decompress ns/B |
|---|---|---|---|
| paq8px_v206fix1 `-12L` | 15,849,084 | 291,916 | 294,847 |
| nncp v3.2 | 14,915,298 | 241,871 | 238,670 |
| cmix v21 | 14,623,723 | 622,949 | 638,442 |

These are the benchmark's rows as of 17 November 2026. Its times are on
`enwik9` and on the machines its notes name.

## 3. The figures

Every figure is drawn by `docs/figures/draw.py` from a CSV beside it, and
both are checked in with the SVG. Each CSV row says where its number comes
from: a commit and a sitting, a section of a stage brief, or the
benchmark. A number appears in a table once; a figure that needs it reads
it from there.

| Figure | Shows | Data | Used in |
|---|---|---|---|
| `understanding` | Bytes per message on the first 30,000,000 messages of day 1: gzip, zstd, xz, `mix9`, the fair baseline, `itch` | README, stage 5 slice table | README, write-up |
| `ratio-speed` | `enwik8` size against ns/byte, compressing and decompressing, two panels | section 2 | README, write-up |
| `stages` | `enwik8` from 100 MB to 61.3, 22.5 and 19.7 | README results | write-up |
| `memory` | Stage 4: ns/byte at levels 12, 16 and 23, before and after | README, stage 4 | write-up |
| `itch-parts` | What each part of the ITCH design is worth, 6.97 to 3.88 bytes a message | stage 5, Measurements | write-up |
| `itch-bytes` | Where the bytes go, by field: half of each message is its timestamp | stage 5, Measurements | write-up |

*Amended in implementation:* `order0` is measured in the sitting and
recorded in the write-up, but not drawn on `ratio-speed`: at 61 MB it
would stretch the size axis to four times the range of every other point.

`understanding` is the slice, not the whole day, because every program on
it, `mix9` included, was measured there by the shipped tools; its caption
gives the whole day's 3.884. It is the quote as a chart: the same coder and
mixer, with and without an understanding of the data.

The script:

- Python 3.8 or later, standard library only. `python3 draw.py` writes
  every figure; no arguments.
- Writes each figure twice, `NAME-light.svg` and `NAME-dark.svg`. The
  README shows them with `<picture>` and `prefers-color-scheme`, which
  GitHub renders.
- The same tables give the same bytes: no timestamps, no generated ids,
  numbers formatted by rule.
- `cpress` in one accent colour, everything else in neutral tones, hollow
  points for numbers not measured here. Text is real SVG text, so it can be
  searched and read by a screen reader; each figure has a `<title>` and a
  `<desc>`.
- Small enough to read top to bottom: one function per figure, shared
  axes and scales, nothing clever.

## 4. The write-up

`docs/write-up.md`. The reader knows how to program and nothing about
compression (style.md, checklist item 7). The model for the tone is *Data
Compression Explained*: plain sentences, an example before a formula,
every claim with a number or a source. Each section can be read alone, and
each ends with what it bought on the benchmark file.

1. **The key to compression.** The quote, what it means, and the project
   in two results: text, where the data is everyone's and the program is
   general; market data, where understanding the data halves the archive.
2. **Compression is prediction.** A bit predicted with probability *p*
   costs −log₂ *p* bits, and an arithmetic coder pays almost exactly that.
   The binary coder, 12-bit probabilities, the order-0 model: 61.3 MB.
3. **Many guesses, one answer.** Context models of orders 1 to 6, hashed
   into tables; mixing in the logistic domain, with weights trained online
   on coding cost; weight sets chosen by context: 22.5 MB. What one weight
   set and smaller tables cost.
4. **Remembering more than a count.** Bit-history states, the probability
   maps that read them, the match model, word contexts, the APMs: 19.7 MB,
   0.3% under lpaq1. What the match model and a smaller table cost.
5. **Waiting for memory.** Why a context mixer is bound by memory, how
   much it was, and what large pages, the encoder's look-ahead and the
   decoder's guess each bought: same archive, 32% and 18% faster.
6. **Understanding the data.** What an ITCH message is, what an order is,
   why a 64-bit reference is nearly incompressible and an order's place in
   its stock's list is not; differences; the order table; where the bytes
   go; the result against the fair baseline, on a second day, and the
   speed it costs to decompress.
7. **Where cpress sits.** The ratio-against-speed figure, lpaq1 beside
   `mix9` on the same machine, and the `enwik9` row.
8. **Why it can be trusted.** One predictor both ways; integers only, so
   archives decode the same everywhere; every archive untrusted; golden
   archives; the compilers and sanitizers in CI. Short.
9. **What is left.** The roadmap's "After 1.0" and stage 5's "Left for
   later", in a paragraph each.

**Appendix: the measurements.** The README's results sections as they
stand, moved verbatim: the stage 1 to 3 rows and experiments, the stage 4
tables, the stage 5 tables. Then the stage 6 text baselines and `enwik9`.
Nothing is restated from memory; every number is copied from the record
that measured it, with its commit, machine and sitting.

**References,** numbered, each with a link where one exists, and each
checked against the source when it is written (style.md, Comments). The
ones the write-up is expected to need:

- Matt Mahoney, *Data Compression Explained*, Dell, 2010–2012, last
  updated 15 April 2013. <https://mattmahoney.net/dc/dce.html>. The quote
  is section 1.5, "Summary". The book permits "cited quotations totaling
  less than one page".
- Large Text Compression Benchmark, Matt Mahoney, and its rules (last
  updated 16 January 2013). <https://mattmahoney.net/dc/text.html>,
  <https://mattmahoney.net/dc/textrules.html>.
- C. E. Shannon, "A Mathematical Theory of Communication", *Bell System
  Technical Journal* 27, 1948: code length as −log₂ *p*.
- I. H. Witten, R. M. Neal and J. G. Cleary, "Arithmetic Coding for Data
  Compression", *Communications of the ACM* 30(6), 1987.
- M. Mahoney, "Adaptive Weighing of Context Models for Lossless Data
  Compression", Florida Tech. Technical Report CS-2005-16, 2005 (PAQ6).
  <https://mattmahoney.net/dc/cs200516.pdf>
- lpaq1, Matt Mahoney, 24 July 2007 (GPL), and paq8l, 8 March 2007.
  <https://mattmahoney.net/dc/#lpaq>. The source of most techniques in
  `mix9`; the code credits name which.
- J. L. Kelly, "A New Interpretation of Information Rate", *Bell System
  Technical Journal* 35(4), 1956: only if the write-up keeps
  [idea.md](../idea.md)'s point about betting.
- Nasdaq TotalView-ITCH 5.0 specification, and the day files
  ([corpus/README.md](../../corpus/README.md)).
- cmix, Byron Knoll, <https://www.byronknoll.com/cmix.html>; nncp,
  Fabrice Bellard, <https://bellard.org/nncp/>: only as the points they
  are on the figure.

A source the write-up does not cite is not listed. No number about
another program appears without its source beside it.

## 5. The README

In this order:

1. The title, then the quote as a block quote with its attribution and a
   link to section 1.5.
2. Two or three short paragraphs: what `cpress` is, the two results, that
   it is written from scratch in C++20 with no dependencies.
3. `understanding`, then `ratio-speed`, each with a one-line caption.
4. A small table of headline numbers: `enwik8` and `enwik9` for `mix9`,
   the ITCH day for `itch` against the fair baseline. Each links to its
   record in the write-up's appendix.
5. **Read the write-up** →
   [docs/write-up.md](../write-up.md).
6. Build, Use, Benchmark: as now. *Amended in implementation:* the
   benchmark section stays whole, since it is the reference for the
   tool's options and its CSV, which the write-up does not repeat; and a
   short list of the five codecs comes before Build, since the status
   paragraph that described them goes.
7. Archive format and codecs: unchanged, since it is the reference.
8. Layout, Design rules, Roadmap, Documentation, Licence.

The status line goes; the roadmap table says where things stand. Nothing
the README says now is lost: what leaves it is in the write-up.

## 6. `enwik9` and the benchmark

**The file.** `enwik9`, from <https://mattmahoney.net/dc/enwik9.zip>,
with its MD5 recorded in `corpus/README.md`. It is 1 GB, and drive C: is
nearly full, so it lives on H: and is passed to `cpress-bench` by path.

**The run.** `cpress-bench --cpu 2 corpus/enwik9`, once, on the MSVC build
the README's Windows rows use, on a quiet machine. A size is a fact and
the benchmark's times are approximate, so one run is enough. At stage 4's
speeds that is about 8 minutes compressing and 10 decompressing.
`cpress-bench` holds the input, the archive and the restored copy at once,
so peak memory is about 3 GiB, 980 MiB of it the model.

**What to expect.** lpaq1 at option 9, with 1.5 GB of model, makes
164,508,919 bytes of `enwik9`; `mix9` is 0.3% under it on `enwik8` with
980 MiB. Expect 160 to 170 MB. A level-23 `mix9` has proportionally less
table per byte on a file ten times larger than `enwik8`, so it may lose
some of that lead.

**The decompressor's size.** The benchmark ranks on the compressed
`enwik9` plus "a zip archive (readable by unzip) containing the
decompressor", which "may contain either an executable program or source
code ... whichever is smaller". Measure both with Info-ZIP `zip -9`:

- the source needed to build `cpress`: `CMakeLists.txt`, `include/`,
  `src/`, about 100 KB at `b9ca88a`;
- the Windows executable, with whatever DLLs it needs that Windows does
  not ship (an MSVC build with the dynamic runtime needs the Visual C++
  runtime).

Submit the smaller; expect it to be the source. The binary needs no
special build.

*Amended in implementation:* `CMakeLists.txt` also builds the tests and
the benchmark, so it cannot configure from `include/` and `src/` alone.
The zip holds those two and a note of the one-line build, which was
checked to build and round-trip with GCC and with MSVC; it was made
with Python's `zipfile` at deflate level 9, a standard zip that `unzip`
reads, since Info-ZIP is not installed: 97,522 bytes. The executable
zips to 89,464 without the Visual C++ runtime it needs, so the source is
smaller.

**The submission** is an email to Matt Mahoney at the address on the
rules page, with what the rules ask for: program and version, `enwik8`
and `enwik9` sizes, options (none: `mix9` is the default), the
decompressor's zip size, approximate times, memory, and the test machine.
The rules require the program to be "available on the Internet for free
download", so the email is sent after the repository is public and
`v1.0` is tagged, and it links to the tag. The owner sends it; the stage
drafts it.

*Amended in implementation:* the draft is written and kept beside the
zip, outside the repository, and is not sent with 1.0. The owner sends
it when content with cpress's performance, with the numbers of the
version sent; the benchmark lists one row for each program, its best,
so a later version would replace an earlier one anyway. On the
benchmark's table of 17 November 2026, 1.0's total would place 61st of
238 programs, and on none of its frontiers: bsc 3.25 is 1.3% smaller in
all, at 23 ns a byte compressing and 8 decompressing on its tester's
machine.

## 7. Licence and `v1.0`

**The licence** must be chosen before the repository is public. Every
line of the project was written for it ([idea.md](../idea.md), Reading: no
code or tables copied from other compressors, several of which are GPL),
so any licence is open to it. What the field uses, each from the
project's own licence file or page on 17 November 2026: zstd, BSD or
GPLv2 at the user's choice; brotli and libdeflate, MIT; libbsc,
Apache-2.0; LZ4, BSD 2-Clause for the library and GPL-2.0-or-later for
the tool; xz, 0BSD for liblzma; ZPAQ, public domain since version 7
(GPL v3 before); lpaq1, GPL; cmix, GPL-3.0. Libraries meant to be built
into other programs are permissive; research compressors are GPL.

- **MIT:** short, understood by everyone, no patent clause.
- **Apache-2.0:** an explicit patent grant and a patent-retaliation
  clause, longer, and a convention of a NOTICE file. Worth having if the
  market-data codec matters to a company that cares about patents.

*Decided by the owner:* **Apache-2.0.** `LICENSE` at the root is the
licence's text exactly as the Apache Software Foundation publishes it,
<https://www.apache.org/licenses/LICENSE-2.0.txt>, and the README says
which licence it is in one line. No per-file headers, and no NOTICE file:
the licence asks for one only if the work has notices to carry, and this
one has none.

**The tag.** `v1.0` on the merge of the last pull request of the stage,
after the licence and before the repository is public. It freezes no new
codec; codecs 1 to 4 are already permanent.

## Exit criteria

- [x] Codec 4's golden archive, made by `v0.5`'s code, in
      `tests/golden_test.cpp`.
- [x] The text baselines on `enwik8`: every program of section 2, its
      version, size and both speeds, in one sitting, with the `cpress`
      sizes equal to the README's. The rows in the commit message and in
      `docs/figures/`.
- [x] Every figure of section 3, drawn by `draw.py` from the checked-in
      tables, light and dark; running the script again changes no byte.
- [x] `docs/write-up.md`, with every section of section 4, the appendix,
      and a reference for every number not measured by this project.
- [x] The README of section 5.
- [x] `enwik9`: the archive's size and CRC-32, both speeds and peak
      memory, and the decompressor's zip size, in the README and the
      write-up.
- [x] `LICENSE`: Apache-2.0, as published.
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan: CI on
      the closing commit is the record.
- [x] clang-format clean, no compiler warnings.
- [x] README, `docs/roadmap.md`, `docs/idea.md` (its reference table is
      the benchmark's, on other machines, and says so), `corpus/README.md`
      and `CLAUDE.md` updated, and the version in `CMakeLists.txt` set to
      1.0.0. *It had said 0.4.0 since stage 4, so `cpress --version` was
      wrong at `v0.5`.*
- [ ] `v1.0` tagged.
- [ ] The repository public. *Amended:* the email to the benchmark is
      drafted and left to the owner, after 1.0 (section 6).

## Suggested commits

1. This brief, and the roadmap, README and `CLAUDE.md` pointing at it.
2. 6a: codec 4's golden archive.
3. 6b: `text-baselines.sh`, the tables, `draw.py` and the figures, with
   the baseline rows in the commit message.
4. 6c: the write-up and the README.
5. 6d: `enwik9`, the licence, and the documents updated for `v1.0`.

*Amended in implementation:* 6b and 6c are one commit. The figures were
drawn for the write-up and checked where it shows them, and the README's
records moved into the write-up in the same edit that took them out.

## What review will look at

- Is every number in the write-up and the README copied from a record
  that measured it, with its commit or its source, and does every
  rounded number round the record's?
- Does any sentence about another program say more than its source
  does? Is every point not measured here marked as such on the figure?
- Does the golden input hold every kind of change to the order table,
  and a byte for the escape, and is it pasted, not generated?
- Does `draw.py` give the same bytes twice, read only the tables, and
  read top to bottom?
- Would a reader new to compression get from the first section to the
  sixth without having to look anything up?
- Did anything leave the README without arriving in the write-up?
