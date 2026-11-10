// cpress-bench: measures size, speed and peak memory for each input file and
// appends one CSV row per file, tagged with the git commit.
//
//   cpress-bench [--codec <name>] [--csv <path>] <file>...
//
// Each file is measured in its own child process (this same executable run
// with --one), so the peak-memory figure belongs to that file alone. The
// figure covers the whole process, including the input and output buffers.
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"
#include "platform.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Options {
    std::string codec{cpress::default_codec().name};
    std::string csv = "bench-results.csv";
    std::string commit;  // internal: passed from parent to child
    std::string one;     // internal: the single file a child measures
    std::vector<std::string> files;
};

void print_usage(std::ostream& os) {
    os << "usage: cpress-bench [--codec <name>] [--csv <path>] <file>...\n"
          "\n"
          "Compresses and decompresses each file, verifies the round trip and\n"
          "appends one row per file to the CSV (default: bench-results.csv).\n";
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](std::string& target) {
            if (i + 1 >= argc) return false;
            target = argv[++i];
            return true;
        };
        if (arg == "--codec") {
            if (!value(options.codec)) return false;
        } else if (arg == "--csv") {
            if (!value(options.csv)) return false;
        } else if (arg == "--commit") {
            if (!value(options.commit)) return false;
        } else if (arg == "--one") {
            if (!value(options.one)) return false;
        } else if (arg.size() > 1 && arg[0] == '-') {
            return false;
        } else {
            options.files.emplace_back(arg);
        }
    }
    return !options.one.empty() || !options.files.empty();
}

// Short commit hash, with "-dirty" if the working tree has changes, or
// "nogit" outside a repository.
std::string git_commit() {
    const std::string discard = std::string{" 2>"} + platform::kNullDevice;
    std::string head = platform::capture("git rev-parse --short HEAD" + discard);
    if (head.empty()) return "nogit";
    if (!platform::capture("git status --porcelain" + discard).empty()) head += "-dirty";
    return head;
}

std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return buffer.data();
}

std::string csv_quote(std::string_view field) {
    std::string out = "\"";
    for (const char c : field) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

double per_byte(Clock::duration elapsed, std::size_t bytes) {
    if (bytes == 0) return 0.0;
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count();
    return ns / static_cast<double>(bytes);
}

constexpr int kNameWidth = 28;

void print_table_header() {
    std::cout << std::left << std::setw(kNameWidth) << "file" << std::right << std::setw(14)
              << "original" << std::setw(14) << "compressed" << std::setw(10) << "bits/B"
              << std::setw(12) << "comp ns/B" << std::setw(12) << "dec ns/B" << std::setw(11)
              << "peak MiB" << "\n";
}

// Child mode: measure one file, print a table row, append a CSV row.
int run_one(const Options& options) {
    const cpress::Codec* codec = cpress::find_codec(std::string_view{options.codec});
    if (codec == nullptr) {
        std::cerr << "cpress-bench: unknown codec '" << options.codec << "'\n";
        return 2;
    }

    const fs::path path = options.one;
    const cpress::Bytes input = cpress::read_file(path);

    const auto t0 = Clock::now();
    const cpress::Bytes archive = cpress::compress(input, *codec);
    const auto t1 = Clock::now();
    const cpress::Bytes restored = cpress::decompress(archive);
    const auto t2 = Clock::now();

    if (restored != input) {
        std::cerr << "cpress-bench: ROUND TRIP MISMATCH for '" << path.string() << "'\n";
        return 1;
    }

    const double bits_per_byte = input.empty() ? 0.0
                                               : 8.0 * static_cast<double>(archive.size()) /
                                                     static_cast<double>(input.size());
    const double compress_ns = per_byte(t1 - t0, input.size());
    const double decompress_ns = per_byte(t2 - t1, input.size());
    const double peak_mib = static_cast<double>(platform::peak_memory_bytes()) / (1024.0 * 1024.0);

    std::cout << std::left << std::setw(kNameWidth) << path.filename().string() << std::right
              << std::setw(14) << input.size() << std::setw(14) << archive.size() << std::fixed
              << std::setprecision(3) << std::setw(10) << bits_per_byte << std::setprecision(1)
              << std::setw(12) << compress_ns << std::setw(12) << decompress_ns << std::setw(11)
              << peak_mib << "\n";

    std::error_code ec;
    const bool needs_header = !fs::exists(options.csv, ec) || fs::file_size(options.csv, ec) == 0;
    std::ofstream csv(options.csv, std::ios::app);
    if (!csv) {
        std::cerr << "cpress-bench: cannot append to '" << options.csv << "'\n";
        return 1;
    }
    if (needs_header) {
        csv << "timestamp_utc,commit,codec,file,original_bytes,compressed_bytes,bits_per_byte,"
               "compress_ns_per_byte,decompress_ns_per_byte,peak_mem_mib\n";
    }
    csv << utc_timestamp() << ',' << options.commit << ',' << codec->name << ','
        << csv_quote(path.generic_string()) << ',' << input.size() << ',' << archive.size() << ','
        << std::fixed << std::setprecision(4) << bits_per_byte << ',' << std::setprecision(2)
        << compress_ns << ',' << decompress_ns << ',' << peak_mib << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        print_usage(std::cerr);
        return 2;
    }

    try {
        if (!options.one.empty()) return run_one(options);

        if (cpress::find_codec(std::string_view{options.codec}) == nullptr) {
            std::cerr << "cpress-bench: unknown codec '" << options.codec << "'\n";
            return 2;
        }

        const std::string commit = git_commit();
        const fs::path self = platform::self_path(argv[0]);
        std::cout << "commit " << commit << ", codec " << options.codec << ", results -> "
                  << options.csv << "\n";
        print_table_header();

        int failures = 0;
        for (const std::string& file : options.files) {
            std::cout << std::flush;
            const int code =
                platform::run_process(self, {"--one", file, "--codec", options.codec, "--csv",
                                             options.csv, "--commit", commit});
            if (code != 0) {
                ++failures;
                std::cerr << "cpress-bench: '" << file << "' failed (exit code " << code << ")\n";
            }
        }
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "cpress-bench: " << e.what() << "\n";
        return 1;
    }
}
