// Mix9 tests: valid predictions on any input, determinism, the level
// arithmetic, and the hints: they change no prediction, and the keys they
// fetch for are the keys the model then selects. Then Mix10Flag's
// shortcuts: when the flag is offered, what a hit and a miss do, and that
// the hints change no byte of its output either. Mix11's and Mix12's keys
// and hints are held to the same tests, and Mix12's word length is checked.
#include "check.hpp"

#include "codec/bitwise.hpp"
#include "model/context_table.hpp"
#include "model/history_table.hpp"
#include "model/mix9.hpp"
#include "model/predictor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace cpress::test {
namespace {

using model::Mix10;
using model::Mix10Flag;
using model::Mix11;
using model::Mix12;
using model::Mix9;

static_assert(model::Predictor<Mix9>);

// level_for_size: the smallest level with 2^level >= size, within the limits.
static_assert(Mix9::level_for_size(0) == Mix9::kMinLevel);
static_assert(Mix9::level_for_size(1) == Mix9::kMinLevel);
static_assert(Mix9::level_for_size(1024) == 10);
static_assert(Mix9::level_for_size(1025) == 11);
static_assert(Mix9::level_for_size(std::uint64_t{1} << 23) == 23);
static_assert(Mix9::level_for_size(std::uint64_t{1} << 40) == Mix9::kMaxLevel);

// memory_bytes: the table in docs/stages/stage-3.md, section 7.
static_assert(Mix9::memory_bytes(10) == 4'534'656);
static_assert(Mix9::memory_bytes(16) == 13'166'976);
static_assert(Mix9::memory_bytes(20) == 140'962'176);
static_assert(Mix9::memory_bytes(22) == 549'906'816);
static_assert(Mix9::memory_bytes(23) == 1'028'057'472);

// Mix10, codec 5's model: an order-7 table more, and a level up to 25.
static_assert(Mix10::kTables == Mix9::kTables + 1);
static_assert(Mix10::level_for_size(std::uint64_t{1} << 40) == 25);
static_assert(Mix10::memory_bytes(10) == 4'555'860);
static_assert(Mix10::memory_bytes(23) == 1'162'280'020);
static_assert(Mix10::memory_bytes(24) == 2'252'799'060);
static_assert(Mix10::memory_bytes(25) == 4'433'837'140);
static_assert(Mix10::table_slot_bits(Mix10::kTables - 1, 24) == 24);

// Mix10Flag, codec 6's model: Mix10 with shortcuts, whose flag tables have
// an eighth of a history table's slots each.
static_assert(Mix10Flag::kTables == Mix10::kTables && Mix10Flag::kMaxLevel == Mix10::kMaxLevel);
static_assert(Mix10Flag::flag_table_bits(Mix10Flag::kMaxLevel) == 22);
static_assert(Mix10Flag::memory_bytes(25) ==
              Mix10::memory_bytes(25) + model::FlagModel::memory_bytes(22));
static_assert(Mix10Flag::memory_bytes(10) == 4'559'804);
static_assert(Mix10Flag::memory_bytes(25) == 4'442'229'436);

// Mix11, codec 7's model: orders 1 to 7 without 6, word, bigram and the two
// class tables; with the match and the bias, twelve inputs. Its mixer is
// 16 MiB, its first APM 4 MiB, and its flag model's mixer a cache line per
// set.
static_assert(Mix11::kOrderOf == std::array{1, 2, 3, 4, 5, 7});
static_assert(Mix11::kTables == 10 && Mix11::kInputs == 12);
static_assert(Mix11::kMaxLevel == Mix10Flag::kMaxLevel);
static_assert(Mix11::table_slot_bits(Mix11::kOrderOf.size() - 1, 24) == 24);  // order 7
static_assert(Mix11::table_slot_bits(Mix11::kTables - 1, 25) == 25);          // a class table
static_assert(Mix11::memory_bytes(10) == 25'622'992);
static_assert(Mix11::memory_bytes(25) == 5'000'147'152);
static_assert(Mix10::kOrderOf == std::array{1, 2, 3, 4, 5, 6, 7});

// Mix12, codec 8's model: Mix11 with order 6 again, so thirteen inputs, and
// a second layer. The layer's two mixers, final mixer and third APM are
// the same size at every level, about 137 MiB.
static_assert(Mix12::kOrderOf == Mix10::kOrderOf);
static_assert(Mix12::kTables == 11 && Mix12::kInputs == 13);
static_assert(Mix12::kMaxLevel == Mix11::kMaxLevel);
static_assert(Mix12::memory_bytes(10) == 168'262'820);
static_assert(Mix12::memory_bytes(25) == 5'679'641'508);

// table_slot_bits: orders 1 and 2 capped, the rest at the level.
static_assert(Mix9::table_slot_bits(0, Mix9::kMaxLevel) == 14);
static_assert(Mix9::table_slot_bits(1, Mix9::kMaxLevel) == 22);
static_assert(Mix9::table_slot_bits(1, 20) == 20);
static_assert(Mix9::table_slot_bits(2, Mix9::kMaxLevel) == Mix9::kMaxLevel);
static_assert(Mix9::table_slot_bits(Mix9::kTables - 1, Mix9::kMaxLevel) == Mix9::kMaxLevel);

// Whether, in a table of 2^slot_bits slots, every order 2 key has a slot of
// its own: no bucket holds more keys than it has ways, and no two keys in a
// bucket share a check. Then no slot is shared or evicted, whatever the
// input.
bool order2_keys_fit(int slot_bits) {
    using model::HistoryTable;
    const HistoryTable table(slot_bits);
    std::vector<std::uint64_t> places;  // bucket and check, one per key
    const auto add = [&](std::uint64_t context, std::uint32_t marker) {
        const auto [bucket, check] = table.locate(model::context_key(context, 2, marker));
        places.push_back((std::uint64_t{bucket} << 8) | check);
    };
    for (std::uint64_t context = 0; context <= 0xFFFF; ++context) {
        add(context, 0);  // the first nibble's slot
        for (std::uint32_t marker = model::kNibbleDone; marker < 2 * model::kNibbleDone; ++marker) {
            add(context, marker);
        }
    }
    CHECK(places.size() == std::size_t{17} * 65'536);
    std::ranges::sort(places);

    std::size_t in_bucket = 0;
    for (std::size_t i = 0; i < places.size(); ++i) {
        const bool same_bucket = i > 0 && (places[i] >> 8) == (places[i - 1] >> 8);
        if (same_bucket && places[i] == places[i - 1]) return false;  // a shared check
        in_bucket = same_bucket ? in_bucket + 1 : 1;
        if (in_bucket > HistoryTable::kWays) return false;
    }
    return true;
}

// The order 2 cap changes no prediction: at its 2^22 slots, as at the 2^23
// it would have at level 23 without it, every key has a slot of its own.
// That rests on the hash, not on the sizing rule, so it is checked here.
void test_order2_cap_changes_nothing() {
    CHECK(order2_keys_fit(Mix9::table_slot_bits(1, Mix9::kMaxLevel)));
    CHECK(order2_keys_fit(Mix9::kMaxLevel));
}

// Random bits are the input a model is worst at, and at the smallest level
// contexts collide constantly. Predictions must stay valid all the same.
void test_predictions_stay_in_range() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(16);
    Mix9 model(Mix9::kMinLevel);
    bool in_range = true;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint32_t p12 = model.predict();
        in_range = in_range && p12 >= model::kProbMin && p12 <= model::kProbMax;
        model.update(static_cast<int>(rng() & 1));
    }
    CHECK(in_range);
}

void test_deterministic() {
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the test reproducible.
    std::mt19937_64 rng(17);
    Mix9 a(Mix9::kMinLevel);
    Mix9 b(Mix9::kMinLevel);
    bool identical = true;
    for (int i = 0; i < 100'000; ++i) {
        identical = identical && a.predict() == b.predict();
        // Bytes with only two bits of entropy, so contexts, words and
        // matches repeat, and every part is exercised beyond its initial
        // state.
        const int bit = (rng() % 4) == 0 ? 1 : 0;
        a.update(bit);
        b.update(bit);
    }
    CHECK(identical);
}

// Stage 3's word step, as Mix9 wrote it before next_words() and
// table_keys() replaced it: the words, and the two word keys before the
// marker, kept up to date on every byte.
struct Stage3Words {
    std::uint64_t word = 0;
    std::uint64_t previous_word = 0;
    std::uint64_t word_key = model::words::word_key(0);
    std::uint64_t bigram_key = model::words::bigram_key(0, 0);

    void push(std::uint32_t byte) {
        if (model::words::is_letter(byte)) {
            word = (word + model::words::lowercase(byte) + 1) * model::words::kWordMultiplier;
        } else if (word != 0) {
            previous_word = word;
            word = 0;
        }
        word_key = model::words::word_key(word);
        bigram_key = model::words::bigram_key(word, previous_word);
    }
};

// The word and bigram tables, the last two.
constexpr std::size_t kWordTable = Mix9::kTables - 2;
constexpr std::size_t kBigramTable = Mix9::kTables - 1;

// next_words() gives the words stage 3's step gave, and next_context() and
// table_keys() the word keys, for every byte value from several starting
// states: no word, within a word, just after one, and within a word of
// UTF-8.
void test_next_words_matches_stage_3() {
    constexpr std::array<std::string_view, 4> kPrefixes{"", "the qu", "the cat ", "Z\xC3\xBCr"};
    constexpr std::uint32_t kMarker = model::kNibbleDone + 3;
    bool matches = true;
    for (const std::string_view prefix : kPrefixes) {
        Stage3Words start3;
        Mix9::ByteContext start;
        for (const char c : prefix) {
            const auto byte = static_cast<std::uint8_t>(c);
            start3.push(byte);
            start = Mix9::next_context(start, byte);
        }
        for (std::uint32_t byte = 0; byte <= 0xFF; ++byte) {
            Stage3Words expected = start3;
            expected.push(byte);
            const model::words::Words words = model::words::next_words(start.words, byte);
            const auto keys = Mix9::table_keys(Mix9::next_context(start, byte), kMarker);
            matches = matches && words.word == expected.word &&
                      words.previous == expected.previous_word &&
                      keys[kWordTable] == ((expected.word_key << model::kMarkerBits) | kMarker) &&
                      keys[kBigramTable] == ((expected.bigram_key << model::kMarkerBits) | kMarker);
        }
    }
    CHECK(matches);
}

// Mix12's word length: the letters since the last byte that is not one,
// counting the transform's codes (128 and up) as letters, as the word
// contexts do.
void test_word_length_counts_letters() {
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 5> kCases{
        {{"", 0}, {"the", 3}, {"the ", 0}, {"a Zebra", 5}, {"x\xC3\xBC", 3}}};
    for (const auto& [prefix, length] : kCases) {
        Mix12::ByteContext context;
        for (const char c : prefix)
            context = Mix12::next_context(context, static_cast<std::uint8_t>(c));
        CHECK(context.word_length == length);
    }
}

// The inputs the hint tests run over: text, where matches run and the
// decoder's guess is often right, and random bytes, where it is not.
std::array<cpress::Bytes, 2> hint_inputs() {
    return {sample_text(8192), random_bytes(8192, 41)};
}

// Hints change nothing. A model given both byte hints before every byte, the
// encoder's with the true bytes, and the decoder's after every bit,
// predicts exactly as one given none.
void test_hints_change_no_prediction() {
    for (const cpress::Bytes& data : hint_inputs()) {
        Mix9 hinted(Mix9::kMinLevel);
        Mix9 plain(Mix9::kMinLevel);
        const auto give_hints = [&](std::size_t i) {
            if (i + 1 < data.size()) hinted.prefetch_after(data[i], data[i + 1]);
            hinted.prefetch_from_match();
        };
        const auto give_bit_hints = [&](int bits) { hinted.prefetch_decoded(bits); };
        CHECK(predictions(hinted, data, give_hints, give_bit_hints) == predictions(plain, data));
    }
}

// Whether every table of `model` has selected the slot of its key in
// `keys`: in the bucket locate() gives the key, with the key's check.
template <typename Model>
bool selects(const Model& model, const std::array<std::uint64_t, Model::kTables>& keys) {
    for (std::size_t i = 0; i < Model::kTables; ++i) {
        const model::HistoryTable& table = model.table(i);
        const model::HistoryTable::Location location = table.locate(keys.at(i));
        if (table.selected_slot() / model::HistoryTable::kWays != location.bucket ||
            table.selected_check() != location.check) {
            return false;
        }
    }
    return true;
}

// Whether two byte contexts are the same: the word keys follow from the
// words.
template <typename Context>
bool same_context(const Context& a, const Context& b) {
    return a.history == b.history && a.words.word == b.words.word &&
           a.words.previous == b.words.previous && a.classes == b.classes &&
           a.word_length == b.word_length;
}

// Starts coding `byte` as the bitwise driver does: a Flagging model may
// code it as a flag first. Whether its bits follow.
template <typename Model>
bool start_byte(Model& model, std::uint8_t byte) {
    if constexpr (model::Flagging<Model>) {
        if (model.offer_flag()) {
            const int hit = byte == model.guess() ? 1 : 0;
            model.update_flag(hit);
            return hit == 0;
        }
    }
    return true;
}

// The keys are the keys. Before each byte, lookahead() from the true bytes,
// which is what prefetch_after() fetches, names the slot every table then
// selects at both nibbles of the byte after, and the context the model is
// in when that byte starts. A byte a flag codes selects no slots.
template <typename Model>
void test_prefetched_keys_are_the_keys_used() {
    for (const cpress::Bytes& data : hint_inputs()) {
        Model model(Model::kMinLevel);
        std::optional<typename Model::Lookahead> ahead;  // for the byte about to be coded
        bool first_nibbles = true;
        bool second_nibbles = true;
        bool contexts = true;
        for (std::size_t i = 0; i < data.size(); ++i) {
            std::optional<typename Model::Lookahead> after;
            if (i + 1 < data.size()) after = model.lookahead(data[i], data[i + 1]);
            if (ahead) contexts = contexts && same_context(ahead->context, model.context());
            if (!start_byte(model, data[i])) {
                ahead = after;
                continue;
            }
            for (int shift = 7; shift >= 0; --shift) {
                if (ahead && shift == 7)
                    first_nibbles = first_nibbles && selects(model, ahead->first_nibble);
                if (ahead && shift == 3)
                    second_nibbles = second_nibbles && selects(model, ahead->second_nibble);
                static_cast<void>(model.predict());
                model.update((data[i] >> shift) & 1);
            }
            ahead = after;
        }
        CHECK(first_nibbles);
        CHECK(second_nibbles);
        CHECK(contexts);
    }
}

// The decoder's candidates include what is used. After the bits
// prefetch_decoded() waits for, the candidate the byte's next bit picks
// is, for the second nibble, the slot every table selects, and, for the
// next byte, the context the model is in when it starts.
template <typename Model>
void test_decoded_candidates_are_the_keys_used() {
    static_assert(Model::kSecondNibbleBits < 4 && Model::kNextByteBits < 8);
    for (const cpress::Bytes& data : hint_inputs()) {
        Model model(Model::kMinLevel);
        bool second_nibbles = true;
        bool next_bytes = true;
        for (const std::uint8_t byte : data) {
            if (!start_byte(model, byte)) continue;
            std::array<std::array<std::uint64_t, Model::kTables>, 2> nibbles{};
            std::array<typename Model::ByteContext, 2> contexts{};
            for (int bits = 1; bits <= 8; ++bits) {
                static_cast<void>(model.predict());
                model.update((byte >> (8 - bits)) & 1);
                if (bits == Model::kSecondNibbleBits) nibbles = model.second_nibble_candidates();
                if (bits == Model::kNextByteBits) contexts = model.next_byte_candidates();
                if (bits == 4) {  // the second nibble's slots are selected now
                    const auto last_bit = static_cast<std::size_t>((byte >> 4) & 1);
                    second_nibbles = second_nibbles && selects(model, nibbles.at(last_bit));
                }
            }
            const auto last_bit = static_cast<std::size_t>(byte & 1);
            next_bytes = next_bytes && same_context(contexts.at(last_bit), model.context());
        }
        CHECK(second_nibbles);
        CHECK(next_bytes);
    }
}

// Text one sentence long, repeated: after its first few copies a long
// match predicts every byte.
cpress::Bytes repeated_sentence(std::size_t count) {
    constexpr std::string_view kSentence = "The match model has seen this sentence before. ";
    cpress::Bytes text;
    while (text.size() < count) text.insert(text.end(), kSentence.begin(), kSentence.end());
    text.resize(count);
    return text;
}

// What a flag did over some input.
struct FlagCount {
    std::size_t offered = 0;
    std::size_t hits = 0;
    bool in_range = true;  // every prediction, of a bit or a flag, was a valid p12
};

// Codes `data` through `model` as the bitwise driver does, without a
// coder: the flag where one is offered, then the byte's bits unless it hit.
FlagCount drive(Mix10Flag& model, cpress::ByteView data) {
    const auto valid = [](std::uint32_t p12) {
        return p12 >= model::kProbMin && p12 <= model::kProbMax;
    };
    FlagCount count;
    for (const std::uint8_t byte : data) {
        if (model.offer_flag()) {
            ++count.offered;
            count.in_range = count.in_range && valid(model.predict_flag());
            const int hit = byte == model.guess() ? 1 : 0;
            count.hits += static_cast<std::size_t>(hit);
            model.update_flag(hit);
            if (hit != 0) continue;
        }
        for (int shift = 7; shift >= 0; --shift) {
            count.in_range = count.in_range && valid(model.predict());
            model.update((byte >> shift) & 1);
        }
    }
    return count;
}

// The flag waits for a long match. Random bytes have none, so no flag is
// ever offered; a repeated sentence has one running almost throughout, and
// most of its bytes are coded as a flag that hits.
void test_flag_waits_for_a_long_match() {
    Mix10Flag on_noise(Mix10Flag::kMinLevel);
    const FlagCount noise = drive(on_noise, random_bytes(65536, 0xF1A6));
    CHECK(noise.offered == 0);
    CHECK(noise.in_range);

    Mix10Flag on_text(Mix10Flag::kMinLevel);
    const cpress::Bytes text = repeated_sentence(65536);
    const FlagCount sentence = drive(on_text, text);
    CHECK(sentence.hits > text.size() / 2);
    CHECK(sentence.in_range);
}

// A model driven over the repeated sentence until it offers a flag, which
// is not yet answered.
struct Offered {
    Mix10Flag model{Mix10Flag::kMinLevel};
    std::uint32_t guess = 0;

    Offered() {
        const cpress::Bytes text = repeated_sentence(65536);
        for (const std::uint8_t byte : text) {
            if (model.offer_flag()) {
                guess = model.guess();
                return;
            }
            for (int shift = 7; shift >= 0; --shift) {
                static_cast<void>(model.predict());
                model.update((byte >> shift) & 1);
            }
        }
    }
};

// A hit is the whole byte: the model's context moves on by the guess, and
// the next byte starts at a byte boundary, where a flag can be offered.
void test_flag_hit_is_the_whole_byte() {
    Offered offered;
    const std::uint64_t before = offered.model.context().history;
    offered.model.update_flag(1);
    CHECK(offered.model.context().history == ((before << 8) | offered.guess));
    static_cast<void>(offered.model.offer_flag());
}

// After a miss, the byte is not the guess. If its first seven bits are
// the guess's, the eighth is forced to the other value, and no other bit
// ever is.
void test_miss_forces_only_the_last_bit() {
    for (const std::uint32_t differ : {1U, 0x80U}) {
        Offered offered;
        Mix10Flag& model = offered.model;
        model.update_flag(0);
        const std::uint32_t byte = offered.guess ^ differ;
        bool forced_as_expected = true;
        for (int shift = 7; shift >= 0; --shift) {
            const int bit = static_cast<int>((byte >> shift) & 1);
            const bool last_and_close = shift == 0 && differ == 1;
            const std::optional<int> forced = model.forced_bit();
            forced_as_expected = forced_as_expected && forced.has_value() == last_and_close &&
                                 (!forced.has_value() || *forced == bit);
            static_cast<void>(model.predict());
            model.update(bit);
        }
        CHECK(forced_as_expected);
        CHECK(!model.forced_bit().has_value());  // exclusion ends with the byte
    }
}

// A Flagging model without its hints: Flagging, but not Prefetching, so
// the driver gives it none.
template <typename Model>
class Unhinted {
public:
    explicit Unhinted(int level) : model_(level) {}
    [[nodiscard]] std::uint32_t predict() noexcept {
        return model_.predict();
    }
    void update(int bit) noexcept {
        model_.update(bit);
    }
    [[nodiscard]] bool offer_flag() noexcept {
        return model_.offer_flag();
    }
    [[nodiscard]] std::uint32_t guess() const noexcept {
        return model_.guess();
    }
    [[nodiscard]] std::uint32_t predict_flag() const noexcept {
        return model_.predict_flag();
    }
    void update_flag(int hit) noexcept {
        model_.update_flag(hit);
    }
    [[nodiscard]] std::optional<int> forced_bit() const noexcept {
        return model_.forced_bit();
    }

private:
    Model model_;
};
static_assert(model::Flagging<Unhinted<Mix10Flag>> && !model::Prefetching<Unhinted<Mix10Flag>>);

// The hints change no byte. The driver gives a Flagging model every hint,
// and its flag a prefetch of its own; without them it codes the same
// payload, and each decodes the other's.
template <typename Model>
void test_flag_hints_change_no_byte() {
    for (const cpress::Bytes& data :
         {repeated_sentence(16384), sample_text(16384), random_bytes(4096, 0x41)}) {
        cpress::Bytes hinted;
        Model encoder(Model::kMinLevel);
        codec::encode_bitwise(encoder, data, hinted);
        cpress::Bytes plain;
        Unhinted<Model> plain_encoder(Model::kMinLevel);
        codec::encode_bitwise(plain_encoder, data, plain);
        CHECK(hinted == plain);

        cpress::Bytes decoded;
        Unhinted<Model> decoder(Model::kMinLevel);
        codec::decode_bitwise(decoder, hinted, data.size(), decoded);
        CHECK(decoded == data);
    }
}

}  // namespace

void run_mix9_tests() {
    test_predictions_stay_in_range();
    test_deterministic();
    test_order2_cap_changes_nothing();
    test_next_words_matches_stage_3();
    test_hints_change_no_prediction();
    test_prefetched_keys_are_the_keys_used<Mix9>();
    test_prefetched_keys_are_the_keys_used<Mix11>();
    test_prefetched_keys_are_the_keys_used<Mix12>();
    test_decoded_candidates_are_the_keys_used<Mix9>();
    test_decoded_candidates_are_the_keys_used<Mix11>();
    test_decoded_candidates_are_the_keys_used<Mix12>();
    test_flag_waits_for_a_long_match();
    test_flag_hit_is_the_whole_byte();
    test_miss_forces_only_the_last_bit();
    test_flag_hints_change_no_byte<Mix10Flag>();
    test_flag_hints_change_no_byte<Mix11>();
    test_flag_hints_change_no_byte<Mix12>();
    test_word_length_counts_letters();
}

}  // namespace cpress::test
