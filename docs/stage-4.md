# Stage 4 brief: performance

Stage 3 built the model. Stage 4 makes the same model faster without
changing a single prediction. Codec 3 is frozen at `v0.3`, so every archive
it writes after this stage must be byte for byte the archive it wrote
before. That is a stronger rule than the roadmap's "within 0.1%", and a
simpler one to check. A speed-up that changes predictions would be a new
codec, and this stage makes none.

Time in `mix9` goes two ways. About 40% is waiting for memory: each byte
selects sixteen slots in history tables of up to 128 MiB each, and the
match model reads a 16 MiB hash table, at addresses that depend on the
byte just coded. The rest is computation. This stage removes most of the
waiting and leaves the computation alone:

- **Large pages,** so a lookup no longer waits for the page table as well
  as for the slot.
- **Looking ahead when compressing.** The encoder has the whole input, so
  it knows every address the next byte will touch while it is still coding
  this one, and can fetch them early.
- **Guessing ahead when decompressing.** The decoder does not know the next
  byte, but the match model usually has a guess; fetching for the guess
  helps when it is right and costs little when it is wrong.

The roadmap's own list, written before `mix9` existed, was measured too and
is mostly not here: aligning buckets and prefetching at the nibble boundary
gained nothing, and a SIMD mixer could gain little (see
[Measurements](#measurements-behind-the-choices)). Its exit number, 870
ns/byte, was met by `v0.3` already, at 779 compressing and 826
decompressing (MSVC).

**Exit number:** on `enwik8`, with the archive identical to `v0.3`'s,
measured on Linux with GCC against `v0.3` built the same way:

- compression at least 25% faster. Expect about 35%: a prototype of this
  design compressed 36% faster.
- decompression at least 10% faster. Expect about 15%: the prototype
  measured 12% from large pages and 4% more from guessing ahead.

A result well short of either means the tables did not get large pages, or
the prefetched keys are not the keys the next byte uses; check those first.
In the prototype, compression ended within about 7% of the same model with
every table in cache, so after this stage it is bound by computation.
Optimising the computation is not this stage's work; issue #4 records what
was measured towards it.

## Scope

In scope:

1. A benchmark that can see a 5% change: pinned, repeated, the minimum
   taken, and the archive's CRC-32 in every row.
2. An allocator for the model's large tables that asks for large pages
   where the operating system grants them without new privileges.
3. Prefetching the next byte's slots, from the input when compressing and
   from the match model's guess when decompressing, through one function.
4. A golden archive for codec 3, which stage 3 froze, and the CRC-32 of its
   `enwik8` archive, which every commit of this stage must reproduce.
5. The memory share of the time, measured without hardware counters, for
   the stage 6 write-up.
6. Tests, and results recorded in the README.

Out of scope, each measured or considered and left for the stage named:

- Any change to a prediction of any codec: no new codec id, no change to
  the container or to any payload.
- Optimising the computation: the mixer (a SIMD mixer included), the APMs,
  the state maps, the coder. Measured in part, and recorded in issue #4 for
  after `v1.0`.
- Threads. One stream is one dependent chain of bits; within it, handing
  work between cores costs as much as a bit takes. Independent blocks
  would change the format and cost 2% to 4% in size; issue #4 has the
  numbers, and stage 5's 13 GB files may want them.
- Aligning buckets to cache lines as a change of its own (measured at
  nothing; it comes free with the allocator, section 2).
- A table layout that keeps a byte's two nibbles in one cache line, or
  anything else that is faster only by predicting differently (a new codec;
  issue #4).
- Profile-guided optimisation (one attempt made it slower; issue #4).
- The ratio-against-speed plot. Its points are not final until stages 5 and
  6 have their numbers, so it moves to stage 6, which draws it once.
- The per-bit loop's one allocation, the encoder's `push_back`. The driver
  reserves ahead and growth is rare.

## Files

```
src/model/table_allocator.hpp  TableAllocator<T>, TableArray<T>: model tables, large pages where granted
src/table_memory.cpp           the operating-system calls behind it, one block per platform
src/model/prefetch.hpp         prefetch(address): the one compiler-specific hint
src/model/predictor.hpp        the Prefetching concept
src/codec/bitwise.hpp          both loops give a Prefetching predictor its hint
src/model/history_table.hpp    slots through the allocator; prefetch(key)
src/model/match_model.hpp      buffer and hash table through the allocator; prefetch_after(history); its guess
src/model/apm.hpp              its table through the allocator
src/model/mix9.hpp             prefetch_after(byte, next), prefetch_from_match(); keys and words written once
bench/bench.cpp                --cpu and --repeat; the minimum; the archive's CRC-32
bench/platform.hpp, .cpp       pin_to_cpu()
tests/table_allocator_test.cpp
tests/history_table_test.cpp   alignment; prefetch and select locate the same bucket
tests/mix9_test.cpp            hints change no prediction; prefetched keys are the keys used
tests/golden_test.cpp          golden archive for codec 3
```

Hot-path code stays header-only. Nothing is added to `include/cpress/`.

## The rule: identical output

Each technique here is chosen because it cannot change a prediction:

- **Large pages** change how an address is translated, not what it holds.
- **A prefetch** is a hint to the cache. It reads nothing the model uses
  and writes nothing. A wrong guess fetches a line that is not needed; it
  never changes a value.

What can change predictions is the code around them: a table no longer
zeroed, a size rounded up and then used as the table's size, a hint
function that moves model state instead of computing from a copy. That
risk is managed by checking output, not by reasoning about it:

- The `order0`, `mix6` and `mix9` checksums in `roundtrip.cpp` do not
  change.
- The CRC-32 of the `enwik8` archive at `v0.3` is recorded in the README
  before any change, and `cpress-bench` reproduces it, with the size
  19,687,838, on every commit of this stage. The samples are at most 1 MiB
  and never fill a level-23 table; `enwik8` does.
- A golden archive for codec 3 (section 5).
- A test that a model given every hint and a model given none predict
  identically (test 8).

## 1. Benchmark discipline

Stage 3 recorded single runs, which vary by about 10% on the development
machine. In the prototype two identical runs, pinned, differed by up to
3.5%, and the same binary measured 650 and 710 ns/byte twenty minutes
apart. Only comparisons made in one sitting, against a baseline run in the
same sitting, mean anything.

The development machine's i9-12900K has performance and efficiency cores,
and a process the scheduler moves to an efficiency core runs much slower.
`cpress-bench` gains two options and a column:

- `--cpu N` pins to logical processor N. Each file is measured in a child
  process (`--one`), so the parent passes the option on and the child pins
  itself before it reads the file: `platform::pin_to_cpu(n)`,
  `SetProcessAffinityMask` on Windows and `sched_setaffinity` on Linux, in
  `bench/platform.cpp` beside the memory query. On the development machine
  logical processors 0 to 15 are performance cores. Under WSL 2 the number
  names a virtual processor, which Windows may move; there it reduces the
  noise without guaranteeing a core.
- `--repeat K` compresses and decompresses K times in the same child and
  reports the minimum of each: the run least disturbed by anything else.
- An `archive_crc32` column: the CRC-32 of the archive, so a row says not
  only how large the output was but whether it was the same output.

The new columns, `repeat` and `archive_crc32`, change the CSV's header.
`bench-results.csv` is a local, untracked log, so the benchmark does not
migrate it: if the file's header is not the one it writes, it refuses to
append and says to move the old file aside.

Every number this stage claims is `--repeat 3`, pinned, with nothing else
running, beside a `v0.3` row from the same sitting. Name the CPU, the
compiler and the operating system beside every row, as the README does.

## 2. Large pages

A page table maps each page of memory to where it really is, and the
processor caches a few thousand of those mappings. With 4 KiB pages, 1 GiB
of tables is 262,144 pages. Almost every lookup misses that cache too and
walks the page table: more dependent memory reads before the slot itself
can be read. With 2 MiB pages the tables are 512 pages, and the walks
mostly hit. Measured: 14% faster compressing, 12% decompressing.

| Platform | How | Needs |
|---|---|---|
| Linux | `std::aligned_alloc` on a 2 MiB boundary, then `madvise(MADV_HUGEPAGE)` before the memory is first touched | transparent huge pages set to `madvise` or `always`, as most distributions ship them; the development machine's WSL 2 kernel has `madvise` |
| Windows | `VirtualAlloc(MEM_LARGE_PAGES)`, falling back to ordinary memory | the "Lock pages in memory" privilege, which an administrator must grant; off by default |
| Elsewhere | ordinary memory | |

```cpp
// Allocates the model's large tables, in the shape of a standard
// allocator's allocate() and deallocate(). Blocks of kLargePageBytes or more
// get large pages where the platform grants them; every block is aligned to
// at least a cache line. Never changes what the memory holds: TableArray
// value-initialises it either way.
template <typename T>
class TableAllocator {
public:
    [[nodiscard]] T* allocate(std::size_t count);         // throws std::bad_alloc
    void deallocate(T* pointer, std::size_t count) noexcept;
};

// A model table: `size` value-initialised elements of a trivial type, in
// memory from TableAllocator. Fixed size; neither copied nor moved.
template <typename T>
class TableArray {
public:
    explicit TableArray(std::size_t size);  // throws std::bad_alloc
    T& operator[](std::size_t i) noexcept;  // and size(), data(), begin(), end()
};
```

The tables were first to be `std::vector<T, TableAllocator<T>>`. Measured,
that made the sanitizer suite twice as slow: libstdc++ value-initialises a
vector with any allocator but `std::allocator` one call per element, which
an unoptimised build pays for in full. Building a level-10 `Mix9` took
32 ms under ASan instead of 7, and the mutation test builds thousands:
the suite took about 580 s, measured while another run shared the
machine, against 224 s for v0.3's suite run alone. Its budget is 5
minutes.
`TableArray` value-initialises in one call, as `std::vector` with
`std::allocator` does, and the build takes 5 ms; a table never grew, so
nothing else is lost. The stage's suite, with its new tests, takes 286 s
under ASan + UBSan, run alone. (Amended in implementation.)

- Failing to get large pages is never an error: the model is the same,
  only slower. Running out of memory is still `std::bad_alloc`, which the
  codec already turns into `cpress::Error`.
- Every block is aligned to 64 bytes at least, so stage 3's buckets become
  single cache lines, as `history_table.hpp` says they will once aligned.
  Update that comment. Alignment alone measured nothing, but costs nothing
  here.
- The history tables, the match model's buffer and hash table, and the
  second APM use it. The mixer's weights and the maps are too small to
  matter. The first APM is the same class as the second, so it uses it
  too; at 16 KiB it takes the ordinary path.
- `memory_bytes()` keeps counting what the model asks for. Rounding up to a
  page is the allocator's business; say so where it rounds.
- Each platform frees a block of a given size the same way whether or not
  it got large pages, so `deallocate` needs only the size: on Windows a
  refused large-page block falls back to `VirtualAlloc` with ordinary
  pages, not to `new`. The test hook, `detail::refuse_large_pages(bool)`,
  takes that same fallback; on Linux it asks for `MADV_NOHUGEPAGE`, so that
  a kernel with transparent huge pages set to `always` does not give large
  pages unasked.
- On Windows, holding the privilege is not enough: `VirtualAlloc` grants
  large pages only to a process that has enabled it in its own token, so
  `table_memory.cpp` does that once, with `AdjustTokenPrivileges`. That
  changes nothing outside the process, and fails harmlessly for an account
  without the privilege. (Amended in implementation: the first draft did
  not say this, and without it a granted privilege would do nothing.)

This is the one place the model touches the operating system. It is
platform code, not a dependency, and the benchmark already has its own;
hard rule 6 is about libraries, and this adds none. The calls live in
`src/table_memory.cpp`, behind one function pair, and nothing else in
`src/model/` knows the platform.

**The trade-off.** The README's numbers are measured on Windows with MSVC,
where large pages need a privilege most machines do not have. Without it,
Windows still gets the look-ahead (section 3), which measured 27% alone,
but not this. The stage's reference rows are therefore Linux (GCC, WSL 2,
same machine), where large pages need nothing, and a reader can reproduce
them without changing a security setting. A Windows row is recorded beside
them, labelled with whether the privilege was granted.

## 3. Prefetching the next byte

The next byte's slots are known only once the byte is. The roadmap planned
to prefetch them at the nibble boundary, which is when they are needed, so
the fetch had no time to hide behind; it measured nothing. Two places know
a byte earlier.

**The encoder knows it exactly.** While byte `i` is coded, bytes `i` and
`i + 1` are both in the input. From them the model can compute every key
the byte after `i` will select: the history and the words after byte `i`,
the first nibble's marker, and the second nibble's marker from the high
nibble of byte `i + 1`. Sixteen history-table buckets and the match model's
hash slot, a whole byte, about 500 ns, before they are needed.

**The decoder can guess it.** When the match model has a match running, it
predicts byte `i` and byte `i + 1`: the bytes that followed last time. On
`enwik8` a match is running before 81% of bytes. Fetching for the guess
costs a few wasted cache lines when the match is wrong.

Both are one function, given two bytes:

```cpp
// A hint, and nothing else: fetch what coding the byte after `byte` will
// touch, if `byte` is the byte now being coded and `next` the one after.
// Computes from a copy of the state; changes nothing the model predicts.
void prefetch_after(std::uint32_t byte, std::uint32_t next) const noexcept;

// prefetch_after() with the match model's guess for both bytes, if a match
// is running; otherwise nothing.
void prefetch_from_match() const noexcept;
```

Both are `const` members, so the compiler, not a reviewer, checks that a
hint cannot move model state. (Amended in implementation: the first draft
of this brief left them non-`const`.)

**The driver.** A new concept beside `Predictor`:

```cpp
template <typename P>
concept Prefetching = Predictor<P> && requires(P& p, std::uint32_t byte) {
    p.prefetch_after(byte, byte);
    p.prefetch_from_match();
};
```

In `encode_bitwise`, before each byte but the last,
`if constexpr (Prefetching<P>) predictor.prefetch_after(input[i], input[i + 1])`;
in `decode_bitwise`, before each byte, `predictor.prefetch_from_match()`.
`Order0` and `Mix6` do not model the concept, so their loops compile as
before. The two loops stay symmetric: each gives the hint it can give, in
the same place.

This makes compression faster than decompression, which context mixing
otherwise is not. `docs/idea.md` says the two cost the same; amend it.

**Written once.** The hint needs the same keys the model selects, for a
state the model is not yet in. Two functions are split out of `Mix9` and
used by both paths, so neither can drift from the other:

```cpp
struct Words { std::uint64_t word; std::uint64_t previous; };
[[nodiscard]] constexpr Words next_words(Words words, std::uint32_t byte) noexcept;

// Mix9: what a byte's keys are made of, besides the marker, and the step
// from one byte's to the next's.
struct ByteContext { std::uint64_t history; words::Words words;
                     std::uint64_t word_key, bigram_key; };
[[nodiscard]] static constexpr ByteContext next_context(const ByteContext& context,
                                                        std::uint32_t byte) noexcept;

// The key of every history table, for a context and marker.
[[nodiscard]] static constexpr std::array<std::uint64_t, kTables> table_keys(
    const ByteContext& context, std::uint32_t marker) noexcept;

// What prefetch_after(byte, next) fetches: every key at both nibbles of
// the byte after `byte`, and the context the match model will hash.
struct Lookahead { std::array<std::uint64_t, kTables> first_nibble, second_nibble;
                   ByteContext context; };
[[nodiscard]] Lookahead lookahead(std::uint32_t byte, std::uint32_t next) const noexcept;
```

`select_slots` becomes `table_keys` and a `select` per table;
`push_word_byte` becomes `next_words`, and `push_byte` steps the context
with `next_context`, as `lookahead` does. The word keys are derived from
`Words` in `next_context` and only there, once a byte, as stage 3 derived
them. (Amended in implementation: the first draft passed the history and
the words separately, which left the history's shift written twice, in
`push_byte` and in the hint. Holding them as one `ByteContext` gives each
step one function. Deriving the word keys in `table_keys` instead would
have cost four more hashes a byte, two per nibble and two in the hint.) `lookahead` is split out
of `prefetch_after` so that test 9 checks the keys the hint actually
fetches, not a test's own derivation of them.)

**The other pieces.**

- `HistoryTable::prefetch(key)` computes the bucket through `locate()`,
  the function `select()` uses.
- `MatchModel::prefetch_after(history)` fetches the hash slot that history
  will select. The prototype also read that slot and fetched the buffer at
  the position it held, the start of the next lookup; measure it alone and
  keep it only if it shows. The read of the slot is a real read, but its
  value is used only as an address to fetch.
- `prefetch(const void*)` in `prefetch.hpp` is `__builtin_prefetch` on GCC
  and Clang, `_mm_prefetch` with `_MM_HINT_T0` on MSVC for x86, and nothing
  elsewhere. It is the one preprocessor conditional in model code; say so
  beside it.
- On GCC it is followed by an empty `asm volatile("")`. GCC counts
  `__builtin_prefetch` as having no effect, so it judged `Mix9::prefetch_after`,
  which only prefetches and was not inlined, a function without effects,
  and deleted every call to it: the first GCC build had no prefetch
  instruction at all, and measured only what large pages gave. The empty
  asm is an effect the compiler must keep; it emits nothing. MSVC kept its
  prefetches without help. A build's prefetches can be counted with
  `objdump -d cpress-bench | grep -c prefetcht0`. (Amended in
  implementation.)

**Distance.** The prototype tried one, two, four, eight and sixteen bytes
ahead; one to four were within noise of each other, and sixteen was worse.
One byte needs no state of its own: the copy is made from the model's
current state. Two bytes ahead for the decoder's guess was no better than
none, so it guesses one.

## 4. Where the time goes

No hardware counters are available on the development machine: Windows
has none without a vendor profiler, and WSL 2 does not give `perf` the
processor's counters. One measurement stands in for the one that matters.
At level 12 every history table fits in the level 2 cache, so the gap
between level 23 and level 12 estimates the cost of missing the cache.
Record it through `cpress-bench`, before and after this stage, for the
stage 6 write-up:

| `v0.3`, compressing | ns/byte |
|---|---|
| Level 23, `enwik8`'s | 701 |
| Level 16 (7.3 MiB of tables) | 473 |
| Level 12 (0.5 MiB of tables) | 441 |

It is an estimate, not a floor: level 12 is a different workload, with far
more evictions and a third more output to code, and the second APM's 4 MiB
still does not fit the level 2 cache. Levels 12 and 16 compress worse and
make different archives; they are a measurement, never a configuration.

If a vendor profiler (Intel VTune, AMD uProf) is to hand, cache misses per
byte go in the commit message beside the timings. They are not an exit
criterion.

## 5. Freezing codec 3

Stage 3 is tagged `v0.3`, so `mix9` and its output are permanent.

- `tests/golden_test.cpp`: a small `mix9` archive as a `constexpr` byte
  array, with its text and the command that produced it, checked in both
  directions as codecs 1 and 2 are.
- The existing `mix9` checksums in `roundtrip.cpp` must not change.
- The README records the CRC-32 of `cpress c corpus/enwik8` at `v0.3`.

## 6. Tests

**Allocator**

1. Blocks below and at least `kLargePageBytes`, and of zero elements,
   allocate and deallocate cleanly; every block is 64-byte aligned.
2. A `TableArray` is zeroed and aligned, at both sizes, with large pages
   granted and refused.
3. With large pages refused (a test hook), a `Mix9` gives the same
   predictions over a few kilobytes as one with them granted.
4. Under ASan, nothing leaks and no deallocation is mismatched, at both
   sizes and through the fallback.

**History table**

5. Every table's first slot is 64-byte aligned, at the smallest and largest
   sizes.
6. `prefetch(key)` and `select(key)` locate the same bucket, for keys
   including 0 and all ones.

**Mix9**

7. `next_words` gives the words stage 3's step gave, for every byte value
   from a few starting states.
8. Hints change nothing. Two models over a few kilobytes of text and of
   random bytes, one given `prefetch_after` with the true bytes and
   `prefetch_from_match` before every byte, one given neither, predict
   identically at every bit.
9. The keys are the keys. Before each byte, `table_keys` for the state
   `prefetch_after` computes from the true bytes equals what the model
   selects at the following byte's two nibbles, for every table, read
   through the test accessors.

**Codec**

10. Checksums for `order0`, `mix6` and `mix9` unchanged; golden archives for
    codecs 1, 2 and 3 decode.
11. The suite stays within stage 3's time budget.

## Measurements behind the choices

From throwaway prototypes, not in the repository: `Mix9` and the coder from
`v0.3`, unchanged except as stated, on `enwik8` (GCC 13, `-O3`, WSL 2 on an
i9-12900K, pinned to one virtual processor). Every row at level 23 made the
same 19,687,817 bytes of coder output, the `v0.3` archive less its header
and level byte. The machine's speed drifted by up to 10% between sittings,
so each table compares rows from one sitting and one binary; read the
changes, not the absolute figures. `cpress-bench` on the real build is
what the exit is judged by.

**The roadmap's plan,** compressing, minimum of two runs:

| Variant | ns/byte | Change |
|---|---|---|
| **`v0.3`** | **701** | |
| Buckets aligned to 64 bytes | 708 | +1% |
| Every bucket prefetched at the nibble boundary, then selected | 718 | +2% |
| Both candidate buckets prefetched one bit early | 712 | +2% |
| 2 MiB pages, which also aligns | 623 | −11% |

Why alignment and the bit-early prefetch measured nothing was not
established. The likeliest reasons are that the processor already fetches
the adjacent cache line with the first, and that out-of-order execution
already overlaps the lookups as well as the prefetches could.

**This design,** compressing, minimum of two runs unless stated:

| Variant | ns/byte | Change |
|---|---|---|
| **`v0.3`** | **752** | |
| History tables shrunk to fit the cache, the match model not | 536 | the tables' wait: about 215 ns |
| The match model shrunk, the tables not | 677 | the match model's wait: about 75 ns |
| 2 MiB pages | 644 | −14% |
| Look-ahead one byte (one run) / two / four / eight / sixteen | 546 / 556 / 570 / 570 / 591 | −27% to −21% |
| Look-ahead four bytes, history tables only | 625 | −17% |
| Look-ahead four bytes, match model only | 743 | −1% |
| **Look-ahead four bytes, with 2 MiB pages** | **483** | **−36%** |
| Level 12, for comparison (a different archive) | 453 | |

Look-ahead one byte with 2 MiB pages was run later, in another sitting,
and measured 474: within noise of four bytes, as without large pages.

The match model's prefetch is worth nothing alone and 9% once the tables'
waits are hidden: until then the processor overlaps it with them.

**Decompression,** a decoder after the encoder in one process, so both
models were held at once (2 GiB); minimum of two runs:

| Variant | ns/byte | Change |
|---|---|---|
| `v0.3` | 770 | |
| 2 MiB pages | 676 | −12% |

**Guessing ahead,** simulated in the encoder exactly as a decoder would do
it, from the match model's state alone, with 2 MiB pages throughout; three
runs of each, interleaved, minimum:

| Variant | ns/byte | Change |
|---|---|---|
| No guess | 576 | |
| **Guess one byte ahead** | **553** | **−4%** |
| Guess two bytes ahead | 579 | +1% |

Paired run by run, the guess was 19 to 34 ns faster each time.

## Exit criteria

- [ ] On `enwik8`, with the archive's size and CRC-32 those of `v0.3`:
      compression at least 25% faster and decompression at least 10%
      faster than `v0.3`, on Linux with GCC, `--repeat 3`, pinned, both
      rows from one sitting. The `cpress-bench` rows in the commit message
      and the README. *Not met yet:* compression 26.1% faster,
      decompression 7.2% (9.7% in an earlier sitting); see the README.
- [x] A Windows row, labelled with whether large pages were granted.
- [x] One row per technique, each added alone to `v0.3`, for the ablation
      table: large pages, look-ahead, guessing ahead.
- [x] The level 12 and level 16 rows (section 4), before and after.
- [ ] All tests pass on MSVC, GCC and Clang, and under ASan + UBSan.
      *MSVC, GCC and ASan + UBSan (GCC) pass locally; Clang is CI-only.*
- [x] clang-format clean, clang-tidy clean, no compiler warnings. *Tidy
      was run on Windows, so the Linux block of `table_memory.cpp` was
      not seen by it.*
- [ ] README (status, results, roadmap), `docs/roadmap.md`, `docs/idea.md`
      and `CLAUDE.md` updated; tag `v0.4`.

## Suggested commits

1. The brief, and the roadmap amended with it.
2. The benchmark options and the golden archive for codec 3, with the
   `v0.3` baseline rows and the `enwik8` CRC-32.
3. Stage 4: the table allocator, the prefetching and their tests;
   `docs/idea.md` amended.
4. Results in the README, citing the commit they were measured at.

## What review will look at

- Does every codec still write exactly the archives it wrote at its tag:
  checksums, golden archives, and `enwik8`'s CRC-32?
- Is each key computed once? Could a prefetch and the select that follows
  it ever disagree, and would a test notice?
- Can a hint change model state? Does `prefetch_after` work on a copy,
  and does `prefetch_from_match` only read?
- Is the platform code confined to `table_memory.cpp` and `prefetch.hpp`,
  with a portable path that does the same thing more slowly?
- Can a failure to get large pages surface as anything but a slower run?
  Can a crafted archive make the decoder allocate more than `kMaxLevel`
  allows, rounding included?
- Is every speed claim a `--repeat 3` minimum, from a real commit, beside a
  `v0.3` row from the same sitting, with the machine, compiler and
  operating system named?
- Would a reader new to the subject understand, from the comments alone,
  what a page walk is, why the encoder can see further ahead than the
  decoder, and why a wrong guess costs nothing but a cache line?
