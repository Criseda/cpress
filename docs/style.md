# cpress code style

The goal is code a stranger can read top to bottom and trust. Formatting is
automatic; this document covers what a formatter cannot decide.

## Tooling

| Tool | Config | When |
|---|---|---|
| clang-format | `.clang-format` | Before every commit |
| clang-tidy | `.clang-tidy` | Before a stage is called done |
| Compiler warnings | `/W4` or `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` | Always; errors in CI (`CPRESS_WERROR`) |
| ASan + UBSan | `CPRESS_SANITIZE` | In CI on every push |

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

## Errors

- Exceptions (`cpress::Error`) are for the boundary: malformed archives, bad
  arguments, I/O failures.
- The per-bit loop never throws and never allocates.
- Anything read from an archive is untrusted. Validate before using it as a
  size, index or count.

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

## Headers

- `#pragma once`.
- Include order: the file's own header, project headers, standard library.
  Blank line between groups.
- Public API lives in `include/cpress/`. Everything else stays in `src/`.

## Tests

- Every codec is covered by the round-trip and corruption tests automatically
  through the codec registry.
- New components get their own direct tests (known vectors, edge cases).
- Once a codec has shipped, a small golden archive is checked in and must
  keep decoding forever.

## Format stability

- A codec is experimental until the stage that introduces it is tagged. From
  the tag on, its id and its output are permanent.
- After that, a change to a model's predictions is a new codec id, never an
  edit to an existing one.
- The header layout is permanent from the first tag.

## Commits

- Small, one idea each, imperative subject line.
- A change that affects compression states the result:
  `Add order-2 model: enwik8 31.2 -> 28.7 MB, 410 -> 520 ns/byte`.

## Review checklist

1. Does it round-trip, and do the corruption tests still pass?
2. Is every new number in the format documented in `cpress.hpp` and the README?
3. Could any input make it read out of bounds, allocate without limit, or loop
   forever?
4. Is there floating point, or anything else that could differ between
   compilers, in the model or coder?
5. Is there a benchmark row for the claim being made?
6. Would a reader new to compression understand why each piece exists?
