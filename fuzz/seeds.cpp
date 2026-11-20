// cpress-fuzz-seeds: writes the fuzzer's first inputs, an archive of each
// sample below through every codec in the registry. A new codec gets its
// seeds without a change here.
//
//   cpress-fuzz-seeds <directory>
//
// The files are named <codec>-<sample>.cpz. The fuzzer starts from valid
// archives, so its first mutations already reach deep into each decoder;
// from random bytes it would spend most of its time on the header.
#include "itch_sample.hpp"

#include "cpress/cpress.hpp"
#include "cpress/io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Sample {
    std::string_view name;
    cpress::Bytes data;
};

// Text whose words repeat, so that codecs 5 and 6 build a dictionary and
// the match models find matches.
cpress::Bytes text_sample() {
    constexpr std::string_view kSentence =
        "The decoder reads what the encoder wrote, and the model predicts each bit of it. ";
    constexpr int kRepeats = 8;
    cpress::Bytes text;
    for (int i = 0; i < kRepeats; ++i) {
        text.insert(text.end(), kSentence.begin(), kSentence.end());
        text.push_back(static_cast<std::uint8_t>('0' + i));
    }
    return text;
}

// Framed ITCH messages, then two bytes that are not a message, so that codec
// 4's archive takes the decoder through the fields of a message, the order
// table and the escape.
cpress::Bytes messages_sample() {
    constexpr std::size_t kMessages = 40;
    cpress::Bytes messages = cpress::test::itch_sample({.messages = kMessages, .seed = 0xF022});
    messages.insert(messages.end(), {0x00, 0x41});
    return messages;
}

cpress::Bytes random_sample() {
    constexpr std::size_t kBytes = 256;
    // NOLINTNEXTLINE(bugprone-random-generator-seed): a fixed seed keeps the seeds reproducible.
    std::mt19937_64 rng(0xF022);
    cpress::Bytes data(kBytes);
    for (std::uint8_t& byte : data) {
        byte = static_cast<std::uint8_t>(rng() >> 56);
    }
    return data;
}

std::vector<Sample> samples() {
    std::vector<Sample> out;
    out.push_back({"empty", {}});
    out.push_back({"text", text_sample()});
    out.push_back({"messages", messages_sample()});
    out.push_back({"random", random_sample()});
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    // argc can be 0: the standard does not promise a program name.
    const std::span<const char* const> arguments(argv, static_cast<std::size_t>(argc));
    if (arguments.size() != 2) {
        std::cerr << "usage: cpress-fuzz-seeds <directory>\n";
        return 2;
    }
    try {
        const fs::path directory = arguments[1];
        fs::create_directories(directory);
        const std::vector<Sample> inputs = samples();
        std::size_t written = 0;
        for (const cpress::Codec& codec : cpress::codecs()) {
            for (const Sample& sample : inputs) {
                const std::string name =
                    std::string(codec.name) + "-" + std::string(sample.name) + ".cpz";
                cpress::write_file(directory / name, cpress::compress(sample.data, codec));
                ++written;
            }
        }
        std::cout << written << " seeds written to " << directory.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "cpress-fuzz-seeds: " << e.what() << "\n";
        return 2;
    }
}
