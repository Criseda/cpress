// cpress command-line tool.
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
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
          "  -f, --force      overwrite the output file if it exists\n"
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
            throw cpress::Error("'" + output.string() + "' already exists (use -f to overwrite)");
        }
    }
}

void report(const fs::path& from, const fs::path& to, std::uint64_t from_size,
            std::uint64_t to_size, double seconds, std::string_view codec) {
    std::cout << from.string() << " -> " << to.string() << "\n  " << from_size << " -> " << to_size
              << " bytes (" << std::fixed << std::setprecision(2) << percent(to_size, from_size)
              << "%), " << seconds << " s, codec " << codec << "\n";
}

int cmd_compress(const Options& options) {
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

    const cpress::Bytes data = cpress::read_file(input);
    const auto start = Clock::now();
    const cpress::Bytes archive = cpress::compress(data, *codec);
    const double seconds = seconds_since(start);
    cpress::write_file(output, archive);

    report(input, output, data.size(), archive.size(), seconds, codec->name);
    return 0;
}

int cmd_decompress(const Options& options) {
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

    const cpress::Bytes archive = cpress::read_file(input);
    const cpress::Header header = cpress::read_header(archive);
    const cpress::Codec* codec = cpress::find_codec(header.codec_id);

    const auto start = Clock::now();
    const cpress::Bytes data = cpress::decompress(archive);
    const double seconds = seconds_since(start);
    cpress::write_file(output, data);

    report(input, output, archive.size(), data.size(), seconds,
           codec != nullptr ? codec->name : std::string_view{"?"});
    return 0;
}

int cmd_info(const Options& options) {
    if (options.paths.size() != 1) {
        throw UsageError("info needs exactly one <archive>");
    }
    const cpress::Bytes archive = cpress::read_file(options.paths[0]);
    const cpress::Header header = cpress::read_header(archive);
    const cpress::Codec* codec = cpress::find_codec(header.codec_id);

    std::cout << "format version  " << static_cast<int>(header.version) << "\n"
              << "codec           " << static_cast<int>(header.codec_id) << " ("
              << (codec != nullptr ? codec->name : std::string_view{"unknown"}) << ")\n"
              << "original size   " << header.original_size << " bytes\n"
              << "archive size    " << archive.size() << " bytes (" << std::fixed
              << std::setprecision(2) << percent(archive.size(), header.original_size) << "%)\n"
              << "crc32           " << std::hex << std::setw(8) << std::setfill('0') << header.crc32
              << "\n";
    return 0;
}

int cmd_codecs() {
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
        if (options.command == "c") return cmd_compress(options);
        if (options.command == "d") return cmd_decompress(options);
        if (options.command == "info") return cmd_info(options);
        if (options.command == "codecs") return cmd_codecs();
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
