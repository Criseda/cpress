#include "codec/bitwise.hpp"
#include "cpress/cpress.hpp"
#include "model/mix6.hpp"
#include "model/order0.hpp"

#include <new>
#include <string>

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

// Codec 2: mix6. Orders 1 to 6, mixed. The payload is one byte, the model's
// memory level, then what the bitwise driver writes. The encoder chooses the
// level from the input size; the decoder uses whatever level the archive
// states, so it never needs to know how the encoder chose. That choice is
// the only difference between the two directions.

// Constructs the model. Its tables are the one allocation sized by a field
// of the archive (see docs/style.md, Errors), so running out of memory for
// them is reported as an Error, like any other archive that cannot be read.
model::Mix6 make_mix6(int level) {
    try {
        return model::Mix6(level);
    } catch (const std::bad_alloc&) {
        constexpr std::uint64_t kMiB = std::uint64_t{1024} * 1024;
        throw Error("not enough memory for mix6 at level " + std::to_string(level) +
                    ": its tables need " + std::to_string(model::Mix6::memory_bytes(level) / kMiB) +
                    " MiB");
    }
}

void mix6_encode(ByteView input, Bytes& output) {
    const int level = model::Mix6::level_for_size(input.size());
    model::Mix6 predictor = make_mix6(level);
    output.push_back(static_cast<std::uint8_t>(level));
    codec::encode_bitwise(predictor, input, output);
}

// The level byte is untrusted: it is checked against the compile-time limits
// before anything is constructed. Any level within them is accepted,
// whatever the original size. The rest of the payload is held to the rules
// of codec::decode_bitwise.
void mix6_decode(ByteView payload, std::uint64_t original_size, Bytes& output) {
    if (payload.empty()) {
        throw Error("corrupt archive: mix6 payload has no level byte");
    }
    const int level = payload.front();
    if (level < model::Mix6::kMinLevel || level > model::Mix6::kMaxLevel) {
        throw Error("corrupt archive: mix6 level " + std::to_string(level) + " is outside " +
                    std::to_string(model::Mix6::kMinLevel) + " to " +
                    std::to_string(model::Mix6::kMaxLevel));
    }
    model::Mix6 predictor = make_mix6(level);
    codec::decode_bitwise(predictor, payload.subspan(1), original_size, output);
}

// New codecs are added here. Ids are part of the file format: never reuse
// or renumber one once an archive using it could exist. The last entry is
// the default.
constexpr std::array<Codec, 3> kCodecs{{
    {.id = 0,
     .name = "stored",
     .summary = "no compression (stage 0 placeholder)",
     .encode = &stored_encode,
     .decode = &stored_decode},
    // Codec 1: order-0 bitwise model. Experimental until stage 1 is tagged.
    {.id = 1,
     .name = "order0",
     .summary = "order-0 bitwise model",
     .encode = &codec::encode_bitwise<model::Order0>,
     .decode = &codec::decode_bitwise<model::Order0>},
    // Codec 2: orders 1 to 6, mixed. Experimental until stage 2 is tagged.
    {.id = 2,
     .name = "mix6",
     .summary = "orders 1-6, logistic mixing",
     .encode = &mix6_encode,
     .decode = &mix6_decode},
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
