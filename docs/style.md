# cpress code style

The goal is code a stranger can read top to bottom and trust. Formatting is
automatic; this document covers what a formatter cannot decide.

## Priorities

When two goals pull apart, the earlier one wins.

1. **Correct.** Every input round-trips, every malformed archive is rejected,
   and nothing relies on undefined or implementation-defined behaviour.
2. **Clear.** A reader who knows C++ but not compression can follow each file
   from top to bottom without running it.
3. **Fast.** Only where `cpress-bench` shows it. Speed that costs clarity
   needs a measurement and a comment saying what was traded.

Code that works but is hard to follow is not finished.

## Tooling

| Tool | Config | When |
|---|---|---|
| clang-format | `.clang-format` | Before every commit |
| clang-tidy | `.clang-tidy` | Before a stage is called done |
| Compiler warnings | `/W4` or `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` | Always; errors in CI (`CPRESS_WERROR`) |
| ASan + UBSan | `CPRESS_SANITIZE` | In CI on every push that changes more than documentation |

## Naming

| Thing | Style | Example |
|---|---|---|
| Types, aliases, template parameters | `PascalCase` | `StateMap`, `ByteView` |
| Functions, variables, parameters | `snake_case` | `read_header`, `bit_count` |
| Private data members | `snake_case_` | `range_`, `weights_` |
| Constants (`constexpr`, global `const`) | `kPascalCase` | `kHeaderSize` |
| Namespaces | `lowercase` | `cpress`, `cpress::model` |
| Files | `snake_case.cpp` / `.hpp` | `state_map.hpp` |
| Macros | Avoid; `UPPER_CASE` if unavoidable | `CHECK` (tests only) |

Name things for what they mean, not how they are stored. Where a value has a
fixed-point scale, the name or type says so: `p12` is a probability in 12
bits, not `p`.

## Readability

- **One job per function.** If a function needs comments to separate its
  parts, the parts are functions. It should fit on a screen.
- **Interface first.** In a class: public interface, then private helpers,
  then data. A reader should meet what a type does before how.
- **Every number with a meaning has a name**, defined once (`kTopByteMask`,
  not `0xFF000000`). Constants that follow from others are computed from
  them, not retyped. A literal is fine where it is the meaning (`>> 8`).
- **Flat control flow.** Handle the exceptional case and return; keep the
  main path unindented. Three levels of nesting is a signal to split.
- **Symmetric things look symmetric.** Encode and decode, read and write are
  laid out so they can be compared line by line.
- **Spell words out.** Abbreviate only what the field itself abbreviates
  (`p12`, `crc`).
- **No cleverness without a reason.** Write the obvious form; if a
  measurement demands a trick, a comment names it and says why.
- **Nothing dead.** No commented-out code, unused parameters or stale
  `TODO`s. Git remembers.

## Language rules

- **C++20, standard library only.** No third-party dependencies.
- **Fixed-width integers** (`std::uint32_t`) in anything that touches the file
  format or model state. `int` and `std::size_t` are for loop counts and sizes.
- **No floating point in model or coder code.** Archives must decode
  bit-identically on every compiler and CPU. Floating point is fine in the
  CLI and benchmark for reporting.
- **No narrowing without a cast.** Every narrowing conversion is a visible
  `static_cast`; no C-style casts.
- **`reinterpret_cast` only at the I/O boundary** (byte buffers to stream
  calls).
- **Views in, values out.** Functions take `std::span` / `std::string_view`
  and return owned values. No owning raw pointers, no `new` / `delete`.
- **`const` by default**, `constexpr` where it is free, `noexcept` on anything
  in the per-bit path.
- **No `using namespace`** outside a function body. Namespace aliases are fine
  in `.cpp` files.
- **Internal linkage** for everything not in a header: anonymous namespace.
  Header-only helpers go in a `detail` namespace.
- **Prove it at compile time.** A `static_assert` or `constexpr` beats a
  runtime check, and a runtime check beats a comment. Templates are
  constrained with concepts; the part of a contract a concept cannot express
  is written beside it.
- **Unsigned arithmetic for bit work.** Shift and mask unsigned types only,
  by less than their width. `std::uint8_t` and `std::uint16_t` promote to
  `int`: compute in `std::uint32_t` and cast on the store.
- **Signed fixed point is the one exception.** A stretched probability or a
  mixer weight is signed, and may be shifted right: C++20 defines that as an
  arithmetic shift. It is never shifted left (multiply instead), and the
  bound that keeps each operation from overflowing is stated beside it.
- **`[[nodiscard]]`** on every function whose result is the reason to call it.
- **`explicit`** on single-argument constructors. Rule of zero: a type that
  needs a destructor or copy operations needs a comment saying why.
- **Standard algorithms and `<bit>`** where they state the intent better than
  a loop (`std::ranges::find_if`, `std::countl_zero`); a plain loop where
  that is clearer.
- **`auto`** when the type is written on the same line or cannot be written.
  Otherwise spell it.

## Errors

- Exceptions (`cpress::Error`) are for the boundary: malformed archives, bad
  arguments, I/O failures.
- The per-bit loop never throws and never allocates.
- Anything read from an archive is untrusted. Validate before using it as a
  size, index or count.
- A malformed archive surfaces as `cpress::Error` and nothing else: not
  `std::bad_alloc`, not `std::length_error`, not a crash or a hang.
- Allocate in proportion to the bytes actually present, never to a size the
  archive claims, even one that passed a sanity check.
- A codec's model tables are the one exception. Their size comes from a
  level stored in the archive, which the decoder checks against a
  compile-time maximum before allocating anything; running out of memory
  for them is reported as `cpress::Error`.

## Invariants

- Write each invariant down where it holds, and `assert` it. An `assert` has
  no side effects.
- Release builds drop `assert`, so an invariant that matters also has a test.
  The test checks the code's own named constant, not a retyped copy of it.

## Performance code

- Model composition is compile-time (templates, concepts). No virtual calls
  and no function pointers inside the per-bit loop.
- Memory layout is part of the design: say in a comment what shares a cache
  line and why.
- An optimisation is not finished until `cpress-bench` shows it. Put the
  before and after numbers in the commit message.
- Prefer the simple version until a measurement says otherwise.

## Comments

- Each file opens with one or two lines saying what it is for.
- Each model component states what it predicts, how much memory it uses, and
  its update rule. Credit the technique it comes from.
- Comment invariants and reasons. Do not narrate what the code already says.
- A comment is part of the code. It changes in the same commit as the code
  it describes; a comment that is no longer true is a bug.
- A bound or figure in a comment says which constant it follows from, so it
  cannot drift when the constant changes.
- Check a credit against the source before writing it.

## Headers

- `#pragma once`.
- Include order: the file's own header, project headers, standard library.
  Blank line between groups.
- Public API lives in `include/cpress/`. Everything else stays in `src/`.

## Tests

- Every codec is covered by the round-trip and corruption tests automatically
  through the codec registry.
- New components get their own direct tests (known vectors, edge cases).
- A test's name says what it proves. Thresholds come from the code's named
  constants or from a derivation in a comment, not from a bare number.
- Untrusted-input tests include archives large enough to reach the limits,
  not only small ones.
- The decoder is fuzzed with libFuzzer in CI (`fuzz/`). An input it finds
  becomes a test in `tests/` that names what it proves, in the commit
  that fixes the bug.
- Once a codec has shipped, a small golden archive is checked in and must
  keep decoding forever.
- CI is the record for compilers and sanitizers: it builds and tests every
  push with MSVC, GCC, Clang and ASan + UBSan, except one that changes only
  Markdown, `docs/` or the licence, which builds nothing. Locally, build
  and run `ctest` with one compiler before pushing; use another only to
  chase a failure CI found.

## Measurement

A measurement decides a claim. Before a long run, name the claim it
decides and the smallest input that decides it; a run that decides none is
not made.

- **A size is a fact of the commit.** Model and coder are deterministic,
  so an archive's size depends on the code and the input, not on the
  machine, the compiler or the load. Measure it once per change to the
  predictions; it stands until they change again. Runs that report only
  sizes can share the machine with each other.
- **A baseline is measured once.** Another program at a fixed version on a
  fixed file makes the same output whatever our code does. Record it with
  the versions, and run it again only when the file, the tool or its
  settings change.
- **A speed needs a quiet machine and little else.** One program at a
  time, pinned, repeated, the minimum taken (stage 4). The input needs to
  be large enough for a steady rate, minutes of work, not the largest file
  there is. Re-measure only the rows a change could have moved.
- **A target is checked where it is set, once.** A target on a whole file
  is run on the whole file at the commit that closes the stage. During
  development a slice is enough.
- **One unseen input tests generalisation.** A second file not used in
  design, with its own fair baseline, says whether the constants were
  fitted to the first. More files say little more.
- **An ablation measured in design stands** while the shipped code makes
  the prototype's size. Re-measure only a part whose code departs from the
  prototype's.

## Format stability

- A codec is experimental until the stage that introduces it is tagged. From
  the tag on, its id and its output are permanent.
- After that, a change to a model's predictions is a new codec id, never an
  edit to an existing one.
- The header layout is permanent from the first tag.

## Commits

- Imperative subject line. A pull request is a few commits, ideally one, not
  many small ones. Large diffs are normal while the project is young.
- A change that affects compression states the result:
  `Add order-2 model: enwik8 31.2 -> 28.7 MB, 410 -> 520 ns/byte`.
- Benchmark numbers come from a clean tree at a real commit, and the README
  cites that hash.

## Briefs

- The brief for a stage is binding. If it turns out to be wrong, amend the
  brief in the same commit as the code that departs from it, with the reason.
  The brief always describes what was built.

## Review checklist

Correct:

1. Does it round-trip, and do the corruption tests still pass?
2. Is every new number in the format documented in `cpress.hpp` and the README?
3. Could any input make it read out of bounds, loop forever, allocate out of
   proportion to its size, or throw anything but `cpress::Error`?
4. Is there floating point, or anything else that could differ between
   compilers, in the model or coder?
5. Is there a benchmark row, from a real commit, for the claim being made?
6. Does the brief still describe what was built?

Clear:

7. Would a reader new to compression understand why each piece exists?
8. Can each function be understood without reading its callers, and each
   file from top to bottom?
9. Is every comment true of the code as it stands: numbers, bounds, credits?
10. Is anything checked at run time, or only stated in a comment, that a
    `static_assert` could prove?
11. Is there a simpler way to write it that says the same thing?
