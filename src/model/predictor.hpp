// The probability convention shared by the coder and every model.
// (The Predictor concept itself arrives with the first model.)
#pragma once

#include <cstdint>

namespace cpress::model {

// A prediction is P(next bit = 1) as an unsigned integer in kProbBits bits.
// Valid values are kProbMin to kProbMax inclusive; 0 and 4096 are never
// produced, so neither symbol ever has zero probability. Variables holding
// one are named with a `p12` suffix or prefix. Bytes are coded most
// significant bit first.
inline constexpr int kProbBits = 12;
inline constexpr std::uint32_t kProbMax = 4095;
inline constexpr std::uint32_t kProbMin = 1;

}  // namespace cpress::model
