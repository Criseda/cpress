// The word transform of codec 5: a reversible rewrite of text that replaces
// the words it uses often with codes of one to three bytes, so that the
// model behind it codes fewer bytes and sees whole words as short tokens.
//
// forward() reads the input twice. The first pass counts every word, a
// maximal run of 2 to 40 ASCII letters in lower case, with a capital first
// letter, or all in capitals; the second writes the transformed stream:
//
//   size  field
//   1     K - 1, where K is the number of code bytes, 1 to 128
//   K     the code bytes, ascending, each 128 or more
//   1     the escape byte
//   1     the capital flag
//   1     the all-capitals flag
//   1     B1, the number of one-byte codes
//   1     B2, the number of lead bytes of two-byte codes; the other
//         K - B1 - B2 lead three-byte codes
//   ...   the dictionary: each word, lower case, then a line feed, in code
//         order; then one more line feed
//   ...   the text
//
// In the text, a word in the dictionary is its code: code byte i < B1 alone
// is word i; a lead byte B1 + j and any code byte c stand for word B1 + j*K
// + c's index; a lead byte B1 + B2 + j and two code bytes stand for the
// three-byte words likewise. A capital or all-capitals word is the flag,
// then the code of its lower-case form. Every other byte is itself, except
// that a byte equal to a code byte, a flag or the escape follows the escape.
//
// Choices, each measured on enwik8 and enwik9 in stage 7 (docs/stages/
// stage-7.md, section 3): the code bytes are the K rarest values of 128 to
// 255, which the model's word contexts already count as letters, so a code
// is a word to them too; the flags and the escape are the rarest control
// bytes; the dictionary holds every word seen at least kMinCount times,
// commonest first by code length, and alphabetical within a code length,
// which compresses the dictionary and gives similar words similar codes.
//
// Technique: the word-replacing transforms of the literature (Skibiński,
// Grabowski and Deorowicz, "Revisiting dictionary-based compression",
// Software: Practice and Experience 35(15), 2005), with the dictionary built
// from the input, as mcm 0.83 does, rather than shipped with the program.
#pragma once

#include "cpress/cpress.hpp"

#include <cstddef>
#include <cstdint>

namespace cpress::text {

// A word is 2 to kMaxWordLetters letters.
inline constexpr std::size_t kMinWordLetters = 2;
inline constexpr std::size_t kMaxWordLetters = 40;

// A word is in the dictionary if it occurs at least this often.
inline constexpr std::uint64_t kMinCount = 16;

// The code layout forward() writes: K code bytes, B1 one-byte codes, B2
// leads of two-byte codes. inverse() reads whatever layout the stream
// states, within the limits above.
inline constexpr std::size_t kCodeBytes = 96;
inline constexpr std::size_t kOneByteCodes = 48;
inline constexpr std::size_t kTwoByteLeads = 40;

// The transformed stream of `input`.
[[nodiscard]] Bytes forward(ByteView input);

// Appends the bytes `transformed` stands for to `output`. They must number
// exactly `original_size`. The stream is untrusted: anything malformed, or
// a stream that would write more or fewer than `original_size` bytes,
// throws Error, and nothing beyond `original_size` bytes is appended first.
void inverse(ByteView transformed, std::uint64_t original_size, Bytes& output);

}  // namespace cpress::text
