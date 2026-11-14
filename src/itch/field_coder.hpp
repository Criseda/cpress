// FieldCoder: codes the symbols a message is made of. It predicts each bit
// of a symbol six times, mixes the six predictions and refines the mix, with
// the parts stages 2 and 3 built for text; what is new is where the
// contexts come from.
//
// A symbol is a nibble or a byte. It has an id, which says what it is (the
// top byte of a timestamp gap; the side of a new order), and four contexts,
// 64-bit numbers the message model chooses: the stock, the last message's
// type, whatever predicts this symbol. A symbol inside a field also has a
// prefix: what came before it in the field.
//
//   inputs      kInputs = 6: a HistoryTable and its state map each, as Mix9's
//   keys        key[0]     = mix(id, 0)                          the symbol alone
//               key[1]     = mix(id, 1, prefix)                  and what came before it
//               key[2 + j] = mix(id, 2 + j, context[j], prefix)  for each context
//   table key   (key << kMarkerBits) | marker, where the marker is 0 for a
//               nibble or a byte's first nibble, and kNibbleDone + the first
//               nibble for a byte's second
//   mixer       weight set 2 * id, or 2 * id + 1 for a byte's second nibble
//   APM         one; context id * 256 + the symbol's bits so far after a
//               leading 1
//   p12         (mixed + refined + 1) / 2: both are in [kProbMin, kProbMax],
//               so their rounded mean is
//
// Numbers. A field is coded as a number: the count of bytes it needs, then
// those bytes, most significant first. Zero is one nibble, and so is "what
// the model expected", since the number coded is a residual (residual.hpp).
// Each byte's id says which byte of the number it is, counted from the least
// significant, so the model knows a byte's weight from its id and everything
// above it from the prefix.
//
// Memory: six history tables of 2^slot_bits slots of 16 bytes, six state
// maps, two weight sets and 256 APM curves per symbol; see memory_bytes().
// The history tables and the APM are on large pages where the platform
// grants them.
//
// Update rule: every map, table, the selected weights and the APM's nearer
// point learn each coded bit; see each component.
//
// Technique: hashed contexts read through bit histories, mixed in the
// logistic domain and refined by an APM, as in Matt Mahoney's lpaq1 (2007);
// the component files say what each takes from it. Coding a number as a
// length and then its bytes, each in the context of the bytes above it, is
// ordinary for structured data; the form here was chosen by measurement
// (docs/stage-5.md).
#pragma once

#include "codec/two_way.hpp"
#include "model/apm.hpp"
#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/logistic.hpp"
#include "model/mixer.hpp"
#include "model/predictor.hpp"
#include "model/probability_map.hpp"
#include "model/state_table.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace cpress::itch {

// What a symbol is: its place in the model's list of symbols.
using Symbol = std::uint32_t;

inline constexpr std::size_t kContextCount = 4;
using Contexts = std::array<std::uint64_t, kContextCount>;

// The ids of a number of `width` bytes: `first` is its length, and
// `first + 1 + k` its byte k, counted from the least significant.
struct NumberField {
    Symbol first;
    int width;

    [[nodiscard]] constexpr Symbol length() const noexcept {
        return first;
    }
    [[nodiscard]] constexpr Symbol byte(int significance) const noexcept {
        return first + 1 + static_cast<Symbol>(significance);
    }
    // How many ids it takes.
    [[nodiscard]] constexpr Symbol ids() const noexcept {
        return static_cast<Symbol>(width) + 1;
    }
};

// The multipliers of mix(): odd 64-bit constants and nothing more. They are
// the values of stage 2's hash multiplier and stage 3's two word keys, which
// the design was measured with; any good hash would do.
inline constexpr std::uint64_t kMixOffset = 0x9E3779B97F4A7C15;
inline constexpr std::uint64_t kMixFirst = 0xD6E8FEB86659FD93;
inline constexpr std::uint64_t kMixSecond = 0x2545F4914F6CDD1D;

// A hash of two words. Unsigned, and wraps.
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t a, std::uint64_t b) noexcept {
    std::uint64_t hash = (a + kMixOffset) * kMixFirst;
    hash ^= hash >> 32;
    hash = (hash ^ b) * kMixSecond;
    return hash ^ (hash >> 29);
}
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t a, std::uint64_t b,
                                          std::uint64_t c) noexcept {
    return mix(mix(a, b), c);
}
[[nodiscard]] constexpr std::uint64_t mix(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                          std::uint64_t d) noexcept {
    return mix(mix(a, b, c), d);
}

// The bytes `u` needs: 0 for 0, 8 for anything of 2^56 or more.
[[nodiscard]] constexpr int bytes_needed(std::uint64_t u) noexcept {
    return (std::bit_width(u) + 7) / 8;
}

class FieldCoder {
public:
    static constexpr std::size_t kInputs = 2 + kContextCount;

    // For number(): model every byte of the number.
    static constexpr int kEveryByte = 8;

    [[nodiscard]] static constexpr std::uint64_t memory_bytes(int slot_bits,
                                                              std::size_t symbols) noexcept {
        std::uint64_t bytes = 0;
        for (std::size_t i = 0; i < kInputs; ++i) {
            bytes += model::HistoryTable::memory_bytes(slot_bits);
            bytes += model::ProbabilityMap::memory_bytes(model::kStateCount);
        }
        bytes += kWeightSetsPerSymbol * symbols * kInputs * sizeof(std::int32_t);  // the mixer
        bytes += model::Apm::memory_bytes(kApmContextsPerSymbol * symbols);
        return bytes;
    }

    // Allocates everything for `symbols` ids, with history tables of
    // 2^slot_bits slots. Throws std::bad_alloc if there is no memory.
    FieldCoder(int slot_bits, std::size_t symbols)
        : tables_(make_tables(slot_bits, std::make_index_sequence<kInputs>())),
          maps_(make_maps(std::make_index_sequence<kInputs>())),
          mixer_(kWeightSetsPerSymbol * symbols),
          apm_(kApmContextsPerSymbol * symbols),
          symbols_(symbols) {}

    // Each of the three below codes a value and returns it. When decoding,
    // the value passed in is ignored.

    // A symbol of four bits, in [0, 15].
    template <codec::TwoWayCoder C>
    [[nodiscard]] std::uint32_t nibble(C& coder, Symbol symbol, const Contexts& contexts,
                                       std::uint64_t prefix, std::uint32_t value) noexcept {
        return code_nibble(coder, symbol, keys(symbol, contexts, prefix), kFirstNibble, 1,
                           kFirstWeights, value);
    }

    // A symbol of eight bits, in [0, 255]: two nibbles, the second in the
    // context of the first.
    template <codec::TwoWayCoder C>
    [[nodiscard]] std::uint32_t byte(C& coder, Symbol symbol, const Contexts& contexts,
                                     std::uint64_t prefix, std::uint32_t value) noexcept {
        const Keys table_keys = keys(symbol, contexts, prefix);
        const std::uint32_t high =
            code_nibble(coder, symbol, table_keys, kFirstNibble, 1, kFirstWeights, value >> 4);
        // The first nibble ended on node kNibbleDone + high: that is the
        // second's marker, and the byte's bits so far after a leading 1.
        const std::uint32_t node = model::kNibbleDone + high;
        const std::uint32_t low =
            code_nibble(coder, symbol, table_keys, node, node, kSecondWeights, value & 0xF);
        return (high << 4) | low;
    }

    // `u`, below 2^(8 * field.width), as its length and then its bytes. Only
    // the first `modelled_bytes` of them are predicted; the rest are coded
    // at one half per bit and touch no table, for bytes with nothing to
    // learn.
    //
    // A decoder can be handed a length the encoder never writes: more bytes
    // than the field has, or a number whose first byte is zero. The length
    // is then cut to the field's width, so nothing is indexed out of range,
    // and valid() turns false.
    template <codec::TwoWayCoder C>
    [[nodiscard]] std::uint64_t number(C& coder, NumberField field, const Contexts& contexts,
                                       std::uint64_t u, int modelled_bytes) noexcept {
        assert(field.width >= 1 && field.width <= 8);
        assert(C::kDecoding || bytes_needed(u) <= field.width);
        auto length = static_cast<int>(nibble(coder, field.length(), contexts, 0,
                                              static_cast<std::uint32_t>(bytes_needed(u))));
        if (length > field.width) {
            valid_ = false;
            length = field.width;
        }

        std::uint64_t number = 0;
        auto prefix = static_cast<std::uint64_t>(length);
        for (int i = 0; i < length; ++i) {
            const int significance = length - 1 - i;
            const auto in = static_cast<std::uint32_t>((u >> (8 * significance)) & 0xFF);
            const std::uint32_t out =
                i < modelled_bytes ? byte(coder, field.byte(significance), contexts, prefix, in)
                                   : unmodelled_byte(coder, in);
            if (i == 0 && out == 0) valid_ = false;  // the length said this byte was needed
            number = (number << 8) | out;
            prefix = mix(prefix, out);
        }
        return number;
    }

    // False once a decoder has seen something no encoder writes. It stays
    // false: the archive is corrupt, and the caller stops.
    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }
    // For the message model, which knows of more such things.
    void mark_invalid() noexcept {
        valid_ = false;
    }

    // For tests.
    [[nodiscard]] const model::HistoryTable& table(std::size_t i) const noexcept {
        return tables_[i];
    }

private:
    using Keys = std::array<std::uint64_t, kInputs>;

    static constexpr std::size_t kWeightSetsPerSymbol = 2;
    static constexpr std::size_t kFirstWeights = 0;
    static constexpr std::size_t kSecondWeights = 1;
    static constexpr std::size_t kApmContextsPerSymbol = 256;

    // The marker of a nibble symbol and of a byte's first nibble; see
    // context_key() in context_table.hpp.
    static constexpr std::uint32_t kFirstNibble = 0;

    [[nodiscard]] static constexpr Keys keys(Symbol symbol, const Contexts& contexts,
                                             std::uint64_t prefix) noexcept {
        Keys table_keys{};
        table_keys[0] = mix(symbol, 0);
        table_keys[1] = mix(symbol, 1, prefix);
        for (std::size_t j = 0; j < kContextCount; ++j) {
            table_keys[2 + j] = mix(symbol, 2 + j, contexts[j], prefix);
        }
        return table_keys;
    }

    // Codes the four bits of `value`. `marker` says which nibble of the
    // symbol this is, `partial` is the symbol's bits so far after a leading
    // 1, and `weights` is kFirstWeights or kSecondWeights.
    template <codec::TwoWayCoder C>
    [[nodiscard]] std::uint32_t code_nibble(C& coder, Symbol symbol, const Keys& table_keys,
                                            std::uint32_t marker, std::uint32_t partial,
                                            std::size_t weights, std::uint32_t value) noexcept {
        assert(symbol < symbols_);
        for (std::size_t i = 0; i < kInputs; ++i) {
            tables_[i].select((table_keys[i] << model::kMarkerBits) | marker);
        }
        mixer_.select((kWeightSetsPerSymbol * symbol) + weights);
        const std::size_t apm_context = kApmContextsPerSymbol * symbol;

        std::uint32_t node = 1;
        for (int shift = 3; shift >= 0; --shift) {
            model::Mixer<kInputs>::Stretched st{};
            for (std::size_t i = 0; i < kInputs; ++i) {
                st[i] = model::stretch(maps_[i].p12(tables_[i].state(node)));
            }
            const std::uint32_t mixed = mixer_.mix(st);
            const std::uint32_t refined = apm_.refine(model::stretch(mixed), apm_context + partial);
            const std::uint32_t p12 = (mixed + refined + 1) / 2;

            int bit = static_cast<int>((value >> shift) & 1);
            coder.code(bit, p12);

            mixer_.update(bit);  // from its own output, not the refined one
            apm_.update(bit);
            for (std::size_t i = 0; i < kInputs; ++i) {
                maps_[i].update(bit);
                tables_[i].update(node, bit);
            }
            node = (node * 2) + static_cast<std::uint32_t>(bit);
            partial = (partial * 2) + static_cast<std::uint32_t>(bit);
        }
        return node - model::kNibbleDone;
    }

    // A byte coded at one half per bit: eight bits, and nothing learned.
    template <codec::TwoWayCoder C>
    [[nodiscard]] static std::uint32_t unmodelled_byte(C& coder, std::uint32_t value) noexcept {
        std::uint32_t byte = 0;
        for (int shift = 7; shift >= 0; --shift) {
            int bit = static_cast<int>((value >> shift) & 1);
            coder.code(bit, model::kProbOne / 2);
            byte = (byte * 2) + static_cast<std::uint32_t>(bit);
        }
        return byte;
    }

    template <std::size_t... Index>
    [[nodiscard]] static std::array<model::HistoryTable, kInputs> make_tables(
        int slot_bits, std::index_sequence<Index...> /*inputs*/) {
        return {(static_cast<void>(Index), model::HistoryTable(slot_bits))...};
    }

    template <std::size_t... Index>
    [[nodiscard]] static std::array<model::ProbabilityMap, kInputs> make_maps(
        std::index_sequence<Index...> /*inputs*/) {
        return {(static_cast<void>(Index), model::make_state_map())...};
    }

    std::array<model::HistoryTable, kInputs> tables_;
    std::array<model::ProbabilityMap, kInputs> maps_;  // the state map of each table
    model::Mixer<kInputs> mixer_;
    model::Apm apm_;
    [[maybe_unused]] std::size_t symbols_;  // every id is below this; only asserts read it
    bool valid_ = true;
};

}  // namespace cpress::itch
