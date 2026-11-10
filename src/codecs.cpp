#include "codec/bitwise.hpp"
#include "cpress/cpress.hpp"
#include "model/order0.hpp"

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

// New codecs are added here. Ids are part of the file format: never reuse
// or renumber one once an archive using it could exist.
constexpr std::array<Codec, 2> kCodecs{{
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
