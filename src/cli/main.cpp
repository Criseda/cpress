// cpress command-line tool.
#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::string_view kVersion = CPRESS_VERSION;

void print_usage(std::ostream& os) {
    os << "cpress " << kVersion
       << " - lossless compressor\n"
          "\n"
          "usage:\n"
          "  cpress c <input> [output]    compress   (default output: <input>.cpz)\n"
          "  cpress d <archive> [output]  decompress (default output: <archive> without .cpz)\n"
          "  cpress info <archive>        show the archive header\n"
          "  cpress codecs                list available codecs\n"
          "\n"
          "options:\n"
          "  --codec <name>   codec to compress with (default: "
       << cpress::default_codec().name
       << ")\n"
          "  -f, --force      replace the output file if it exists\n"
          "  -h, --help       show this help\n"
          "  --version        show the version\n";
}

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string command;
    std::vector<std::string> paths;
    std::string codec{cpress::default_codec().name};
    bool force = false;
};

Options parse(const std::vector<std::string>& args) {
    Options options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-f" || arg == "--force") {
            options.force = true;
        } else if (arg == "--codec") {
            if (i + 1 >= args.size()) throw UsageError("--codec needs a name");
            options.codec = args[++i];
        } else if (arg.size() > 1 && arg[0] == '-') {
            throw UsageError("unknown option '" + arg + "'");
        } else if (options.command.empty()) {
            options.command = arg;
        } else {
            options.paths.push_back(arg);
        }
    }
    return options;
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

double percent(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

void check_output(const fs::path& input, const fs::path& output, bool force) {
    std::error_code ec;
    if (fs::exists(output, ec)) {
        if (fs::equivalent(input, output, ec)) {
            throw cpress::Error("input and output are the same file");
        }
        if (!force) {
            throw cpress::Error("'" + output.string() + "' already exists (use -f to replace it)");
        }
    }
}

// The header of the archive at `path`, read without the rest of the file:
// an archive can be larger than memory.
cpress::Header read_header_of(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw cpress::Error("cannot open '" + path.string() + "'");
    std::array<std::uint8_t, cpress::kHeaderSize> header{};
    file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto count = static_cast<std::size_t>(file.gcount());
    return cpress::read_header(std::span(header).first(count));
}

void report(const fs::path& from, const fs::path& to, std::uint64_t from_size,
            std::uint64_t to_size, double seconds, std::string_view codec) {
    std::cout << from.string() << " -> " << to.string() << "\n  " << from_size << " -> " << to_size
              << " bytes (" << std::fixed << std::setprecision(2) << percent(to_size, from_size)
              << "%), " << seconds << " s, codec " << codec << "\n";
}

int run_compress(const Options& options) {
    if (options.paths.empty() || options.paths.size() > 2) {
        throw UsageError("c needs <input> and an optional [output]");
    }
    const cpress::Codec* codec = cpress::find_codec(std::string_view{options.codec});
    if (codec == nullptr) {
        throw UsageError("unknown codec '" + options.codec + "' (see: cpress codecs)");
    }

    const fs::path input = options.paths[0];
    fs::path output;
    if (options.paths.size() == 2) {
        output = options.paths[1];
    } else {
        output = input;
        output += std::string{cpress::kExtension};
    }
    check_output(input, output, options.force);

    // File to file: a codec that can stream never holds the input. The time
    // includes reading and writing the files.
    const auto start = Clock::now();
    cpress::compress_file(input, output, *codec);
    const double seconds = seconds_since(start);

    report(input, output, fs::file_size(input), fs::file_size(output), seconds, codec->name);
    return 0;
}

int run_decompress(const Options& options) {
    if (options.paths.empty() || options.paths.size() > 2) {
        throw UsageError("d needs <archive> and an optional [output]");
    }

    const fs::path input = options.paths[0];
    fs::path output;
    if (options.paths.size() == 2) {
        output = options.paths[1];
    } else if (input.extension() == fs::path{std::string{cpress::kExtension}}) {
        output = input;
        output.replace_extension();
    } else {
        output = input;
        output += ".out";
    }
    check_output(input, output, options.force);

    const auto start = Clock::now();
    cpress::decompress_file(input, output);
    const double seconds = seconds_since(start);

    const cpress::Header header = read_header_of(input);
    const cpress::Codec* codec = cpress::find_codec(header.codec_id);
    report(input, output, fs::file_size(input), fs::file_size(output), seconds,
           codec != nullptr ? codec->name : std::string_view{"?"});
    return 0;
}

int run_info(const Options& options) {
    if (options.paths.size() != 1) {
        throw UsageError("info needs exactly one <archive>");
    }
    const fs::path archive = options.paths[0];
    const cpress::Header header = read_header_of(archive);
    const cpress::Codec* codec = cpress::find_codec(header.codec_id);
    const std::uint64_t archive_size = fs::file_size(archive);

    std::cout << "format version  " << static_cast<int>(header.version) << "\n"
              << "codec           " << static_cast<int>(header.codec_id) << " ("
              << (codec != nullptr ? codec->name : std::string_view{"unknown"}) << ")\n"
              << "original size   " << header.original_size << " bytes\n"
              << "archive size    " << archive_size << " bytes (" << std::fixed
              << std::setprecision(2) << percent(archive_size, header.original_size) << "%)\n"
              << "crc32           " << std::hex << std::setw(8) << std::setfill('0') << header.crc32
              << "\n";
    return 0;
}

int run_codecs() {
    for (const cpress::Codec& codec : cpress::codecs()) {
        std::cout << static_cast<int>(codec.id) << "  " << codec.name << "  " << codec.summary
                  << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);

    for (const std::string& arg : args) {
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "cpress " << kVersion << "\n";
            return 0;
        }
    }

    try {
        const Options options = parse(args);
        if (options.command == "c") return run_compress(options);
        if (options.command == "d") return run_decompress(options);
        if (options.command == "info") return run_info(options);
        if (options.command == "codecs") return run_codecs();
        if (options.command.empty()) throw UsageError("no command given");
        throw UsageError("unknown command '" + options.command + "'");
    } catch (const UsageError& e) {
        std::cerr << "cpress: " << e.what() << "\n\n";
        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "cpress: " << e.what() << "\n";
        return 1;
    }
}
