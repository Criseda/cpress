#include "codec/bitwise.hpp"
#include "codec/buffer_stream.hpp"
#include "codec/messages.hpp"
#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"
#include "itch/message_model.hpp"
#include "itch/order_table.hpp"
#include "model/mix6.hpp"
#include "model/mix9.hpp"
#include "model/order0.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
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

// Codecs 2, 3 and 4: models with a memory level. The payload is one byte,
// the level, then what the codec's driver writes. The encoder chooses the level
// from the input size; the decoder uses whatever level the archive states,
// so it never needs to know how the encoder chose. That choice is the only
// difference between the two directions.
//
// The functions below are written once for every such model, so the rules
// for the level byte exist once. `Name` is the codec's name, for messages.

// What the level functions need of a model: that it is constructed from a
// level, the limits on that level, and the two functions of it.
template <typename Model>
concept Levelled = std::constructible_from<Model, int> && requires(std::uint64_t size, int level) {
    { Model::kMinLevel } -> std::convertible_to<int>;
    { Model::kMaxLevel } -> std::convertible_to<int>;
    { Model::level_for_size(size) } -> std::same_as<int>;
    { Model::memory_bytes(level) } -> std::same_as<std::uint64_t>;
};

// A levelled model the bitwise driver can run.
template <typename Model>
concept LevelledModel = model::Predictor<Model> && Levelled<Model>;

// Constructs the model. Its tables are the one allocation sized by a field
// of the archive (see docs/style.md, Errors), so running out of memory for
// them is reported as an Error, like any other archive that cannot be read.
template <Levelled Model, const std::string_view& Name>
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

// The level a payload states. It is untrusted: it is checked against the
// compile-time limits before anything is constructed. Any level within
// them is accepted, whatever the original size.
template <Levelled Model, const std::string_view& Name>
int level_of(ByteView payload) {
    if (payload.empty()) {
        throw Error("corrupt archive: " + std::string(Name) + " payload has no level byte");
    }
    const int level = payload.front();
    if (level < Model::kMinLevel || level > Model::kMaxLevel) {
        throw Error("corrupt archive: " + std::string(Name) + " level " + std::to_string(level) +
                    " is outside " + std::to_string(Model::kMinLevel) + " to " +
                    std::to_string(Model::kMaxLevel));
    }
    return level;
}

// The rest of the payload is held to the rules of codec::decode_bitwise.
template <LevelledModel Model, const std::string_view& Name>
void levelled_decode(ByteView payload, std::uint64_t original_size, Bytes& output) {
    auto predictor = make_levelled_model<Model, Name>(level_of<Model, Name>(payload));
    codec::decode_bitwise(predictor, payload.subspan(1), original_size, output);
}

// Codec 2: orders 1 to 6, mixed.
constexpr std::string_view kMix6Name = "mix6";

// Codec 3: bit histories, a match model, word contexts and two APMs.
constexpr std::string_view kMix9Name = "mix9";

// Codec 4: ITCH 5.0 messages, field by field against an order table. The
// one codec that streams: its benchmark is a file of 8.25 GB.
constexpr std::string_view kItchName = "itch";

// The orders evicted by every finished compression; see order_table.hpp.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): a counter for the benchmark.
std::atomic<std::uint64_t> g_evictions_recorded{0};

void itch_encode_stream(ByteSource& input, std::uint64_t size, ByteSink& output) {
    const int level = itch::MessageModel::level_for_size(size);
    auto model = make_levelled_model<itch::MessageModel, kItchName>(level);
    const std::array<std::uint8_t, 1> level_byte{static_cast<std::uint8_t>(level)};
    output.write(level_byte);
    static_cast<void>(codec::encode_messages(model, input, output));
    g_evictions_recorded += model.evictions();
}

// The rest of the payload is held to the rules of codec::decode_messages.
void itch_decode_stream(ByteView payload, std::uint64_t original_size, ByteSink& output) {
    const int level = level_of<itch::MessageModel, kItchName>(payload);
    auto model = make_levelled_model<itch::MessageModel, kItchName>(level);
    codec::decode_messages(model, payload.subspan(1), original_size, output);
}

// The same two on whole buffers.
void itch_encode(ByteView input, Bytes& output) {
    codec::SpanSource source(input);
    codec::VectorSink sink(output);
    itch_encode_stream(source, input.size(), sink);
}

// Memory follows the bytes actually decoded: the vector grows as the
// decoder hands them over, a block at a time.
void itch_decode(ByteView payload, std::uint64_t original_size, Bytes& output) {
    codec::VectorSink sink(output);
    itch_decode_stream(payload, original_size, sink);
}

// The codec used when none is named.
constexpr std::uint8_t kDefaultCodecId = 3;

// New codecs are added here. Ids are part of the file format: never reuse
// or renumber one once an archive using it could exist.
constexpr std::array<Codec, 5> kCodecs{{
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
    // Codec 4: experimental until the stage is tagged v0.5.
    {.id = 4,
     .name = kItchName,
     .summary = "ITCH 5.0 messages, field by field against an order table",
     .encode = &itch_encode,
     .decode = &itch_decode,
     .encode_stream = &itch_encode_stream,
     .decode_stream = &itch_decode_stream},
}};

// The default is one of them.
static_assert(std::ranges::any_of(kCodecs,
                                  [](const Codec& codec) { return codec.id == kDefaultCodecId; }));

}  // namespace

namespace itch {

std::uint64_t evictions_recorded() noexcept {
    return g_evictions_recorded.load();
}

}  // namespace itch

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
    return *find_codec(kDefaultCodecId);
}

}  // namespace cpress
