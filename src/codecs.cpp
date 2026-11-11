#include "codec/bitwise.hpp"
#include "cpress/cpress.hpp"
#include "model/mix6.hpp"
#include "model/mix9.hpp"
#include "model/order0.hpp"

#include <concepts>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>

namespace cpress {
namespace {

// Codec 0: stored. No compression. It exists so the container, CLI, tests
// and benchmark runner can be proven end to end before any model is written.
void stored_encode(ByteView input, Bytes& output) {
    output.reserve(output.size() + input.size());
    output.insert(output.end(), input.begin(), input.end());
}

void stored_decode(ByteView payload, std::uint64_t original_size, Bytes& output) {
    if (payload.size() != original_size) {
        throw Error("corrupt archive: stored payload size does not match the header");
    }
    output.insert(output.end(), payload.begin(), payload.end());
}

// Codecs 2 and 3: models with a memory level. The payload is one byte, the
// level, then what the bitwise driver writes. The encoder chooses the level
// from the input size; the decoder uses whatever level the archive states,
// so it never needs to know how the encoder chose. That choice is the only
// difference between the two directions.
//
// The functions below are written once for every such model, so the rules
// for the level byte exist once. `Name` is the codec's name, for messages.

// What the level functions need of a model: a Predictor constructed from a
// level, the limits on that level, and the two functions of it.
template <typename Model>
concept LevelledModel = model::Predictor<Model> && std::constructible_from<Model, int> &&
                        requires(std::uint64_t size, int level) {
                            { Model::kMinLevel } -> std::convertible_to<int>;
                            { Model::kMaxLevel } -> std::convertible_to<int>;
                            { Model::level_for_size(size) } -> std::same_as<int>;
                            { Model::memory_bytes(level) } -> std::same_as<std::uint64_t>;
                        };

// Constructs the model. Its tables are the one allocation sized by a field
// of the archive (see docs/style.md, Errors), so running out of memory for
// them is reported as an Error, like any other archive that cannot be read.
template <LevelledModel Model, const std::string_view& Name>
Model make_levelled_model(int level) {
    try {
        return Model(level);
    } catch (const std::bad_alloc&) {
        constexpr std::uint64_t kMiB = std::uint64_t{1024} * 1024;
        throw Error("not enough memory for " + std::string(Name) + " at level " +
                    std::to_string(level) + ": its tables need " +
                    std::to_string(Model::memory_bytes(level) / kMiB) + " MiB");
    }
}

template <LevelledModel Model, const std::string_view& Name>
void levelled_encode(ByteView input, Bytes& output) {
    const int level = Model::level_for_size(input.size());
    auto predictor = make_levelled_model<Model, Name>(level);
    output.push_back(static_cast<std::uint8_t>(level));
    codec::encode_bitwise(predictor, input, output);
}

// The level byte is untrusted: it is checked against the compile-time limits
// before anything is constructed. Any level within them is accepted,
// whatever the original size. The rest of the payload is held to the rules
// of codec::decode_bitwise.
template <LevelledModel Model, const std::string_view& Name>
void levelled_decode(ByteView payload, std::uint64_t original_size, Bytes& output) {
    if (payload.empty()) {
        throw Error("corrupt archive: " + std::string(Name) + " payload has no level byte");
    }
    const int level = payload.front();
    if (level < Model::kMinLevel || level > Model::kMaxLevel) {
        throw Error("corrupt archive: " + std::string(Name) + " level " + std::to_string(level) +
                    " is outside " + std::to_string(Model::kMinLevel) + " to " +
                    std::to_string(Model::kMaxLevel));
    }
    auto predictor = make_levelled_model<Model, Name>(level);
    codec::decode_bitwise(predictor, payload.subspan(1), original_size, output);
}

// Codec 2: orders 1 to 6, mixed.
constexpr std::string_view kMix6Name = "mix6";

// Codec 3: bit histories, a match model, word contexts and two APMs.
constexpr std::string_view kMix9Name = "mix9";

// New codecs are added here. Ids are part of the file format: never reuse
// or renumber one once an archive using it could exist. The last entry is
// the default.
constexpr std::array<Codec, 4> kCodecs{{
    {.id = 0,
     .name = "stored",
     .summary = "no compression (stage 0 placeholder)",
     .encode = &stored_encode,
     .decode = &stored_decode},
    // Codec 1: order-0 bitwise model. Frozen at v0.1: its output is permanent.
    {.id = 1,
     .name = "order0",
     .summary = "order-0 bitwise model",
     .encode = &codec::encode_bitwise<model::Order0>,
     .decode = &codec::decode_bitwise<model::Order0>},
    // Codec 2: orders 1 to 6, mixed. Frozen at v0.2: its output is permanent.
    {.id = 2,
     .name = kMix6Name,
     .summary = "orders 1-6, logistic mixing",
     .encode = &levelled_encode<model::Mix6, kMix6Name>,
     .decode = &levelled_decode<model::Mix6, kMix6Name>},
    // Codec 3: orders 1 to 6, words and a match model, mixed and refined.
    // Frozen at v0.3: its output is permanent.
    {.id = 3,
     .name = kMix9Name,
     .summary = "bit histories, match model, word contexts, APM",
     .encode = &levelled_encode<model::Mix9, kMix9Name>,
     .decode = &levelled_decode<model::Mix9, kMix9Name>},
}};

}  // namespace

std::span<const Codec> codecs() noexcept {
    return kCodecs;
}

const Codec* find_codec(std::uint8_t id) noexcept {
    for (const Codec& codec : kCodecs) {
        if (codec.id == id) return &codec;
    }
    return nullptr;
}

const Codec* find_codec(std::string_view name) noexcept {
    for (const Codec& codec : kCodecs) {
        if (codec.name == name) return &codec;
    }
    return nullptr;
}

const Codec& default_codec() noexcept {
    return kCodecs.back();
}

}  // namespace cpress
