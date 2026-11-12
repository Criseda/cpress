// cpress-bench: measures size, speed and peak memory for each input file and
// appends one CSV row per file, tagged with the git commit.
//
//   cpress-bench [--codec <name>] [--csv <path>] [--cpu <n>] [--repeat <k>] <file>...
//
// Each file is measured in its own child process (this same executable run
// with --one), so the peak-memory figure belongs to that file alone. The
// figure covers the whole process, including the input and output buffers.
//
// Timings on a desktop vary by several percent from run to run, more on a
// processor with cores of two speeds. --cpu pins the child to one logical
// processor, and --repeat runs the round trip k times in the child and
// reports the fastest of each direction: the run least disturbed by anything
// else. Every row carries the archive's CRC-32, so it says whether the
// output was the same as well as how large it was, and how much of each
// model the operating system put on large pages, which the speed depends on
// and the program cannot choose.
#include "cpress/cpress.hpp"
#include "cpress/io.hpp"
#include "model/table_allocator.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// More repeats than this is a mistake, not a measurement.
constexpr int kMaxRepeat = 100;

struct Options {
    std::string codec{cpress::default_codec().name};
    std::string csv = "bench-results.csv";
    std::optional<int> cpu;  // the logical processor to pin to, if any
    int repeat = 1;
    std::string commit;  // internal: passed from parent to child
    std::string one;     // internal: the single file a child measures
    std::vector<std::string> files;
};

void print_usage(std::ostream& os) {
    os << "usage: cpress-bench [--codec <name>] [--csv <path>] [--cpu <n>] [--repeat <k>] "
          "<file>...\n"
          "\n"
          "Compresses and decompresses each file, verifies the round trip and\n"
          "appends one row per file to the CSV (default: bench-results.csv).\n"
          "\n"
          "  --cpu <n>     run on logical processor n only\n"
          "  --repeat <k>  run the round trip k times (1 to 100) and report the\n"
          "                fastest of each direction (default 1)\n";
}

// Parses all of `text` as a non-negative integer.
std::optional<int> parse_count(std::string_view text) {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0) return {};
    return value;
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        auto value = [&](std::string& target) {
            if (i + 1 >= argc) return false;
            target = argv[++i];
            return true;
        };
        auto count = [&](std::optional<int>& target) {
            std::string text;
            if (!value(text)) return false;
            target = parse_count(text);
            return target.has_value();
        };
        if (arg == "--codec") {
            if (!value(options.codec)) return false;
        } else if (arg == "--csv") {
            if (!value(options.csv)) return false;
        } else if (arg == "--cpu") {
            if (!count(options.cpu)) return false;
        } else if (arg == "--repeat") {
            std::optional<int> repeat;
            if (!count(repeat) || *repeat < 1 || *repeat > kMaxRepeat) return false;
            options.repeat = *repeat;
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

// The CSV's first line. A file that starts with any other line was written
// by another version of the benchmark, and rows are never appended to it.
constexpr std::string_view kCsvHeader =
    "timestamp_utc,commit,codec,file,original_bytes,compressed_bytes,archive_crc32,"
    "bits_per_byte,compress_ns_per_byte,decompress_ns_per_byte,repeat,peak_mem_mib,"
    "large_page_mib";

// True if rows can be appended to `path`: it is missing, empty, or starts
// with kCsvHeader. Otherwise says why not.
bool csv_accepts_rows(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) == 0) return true;
    std::ifstream csv(path);
    std::string first_line;
    std::getline(csv, first_line);
    if (!first_line.empty() && first_line.back() == '\r') first_line.pop_back();
    if (first_line == kCsvHeader) return true;
    std::cerr << "cpress-bench: '" << path.string()
              << "' has the columns of another version of cpress-bench; move it aside or pass "
                 "--csv <new file>\n";
    return false;
}

// The bytes of memory put on large pages so far, by whichever count the
// platform keeps: the model's own on Windows, where each block is granted
// large pages or not when it is allocated; the kernel's on Linux, which
// backs a block with them page by page as it is touched, and counts them
// for every process at once.
std::uint64_t large_page_bytes_so_far() {
    return cpress::model::large_page_bytes_granted() + platform::huge_page_bytes_faulted();
}

double mebibytes(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double per_byte(Clock::duration elapsed, std::size_t bytes) {
    if (bytes == 0) return 0.0;
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count();
    return ns / static_cast<double>(bytes);
}

// The archive's size and CRC-32, and the fastest compression and
// decompression of `repeat`.
struct Measurement {
    std::size_t archive_size = 0;
    std::uint32_t archive_crc32 = 0;
    Clock::duration compress = Clock::duration::max();
    Clock::duration decompress = Clock::duration::max();
};

// Compresses and decompresses `input` `repeat` times, keeping the fastest
// time of each. Every round trip must restore the input and every archive
// must have the first one's size and CRC-32; otherwise returns nothing,
// having said which. Each run's archive is freed before the next, so the
// peak memory of a row does not depend on `repeat`.
std::optional<Measurement> measure(const cpress::Bytes& input, const cpress::Codec& codec,
                                   int repeat) {
    Measurement result;
    for (int run = 0; run < repeat; ++run) {
        const auto t0 = Clock::now();
        const cpress::Bytes archive = cpress::compress(input, codec);
        const auto t1 = Clock::now();
        const cpress::Bytes restored = cpress::decompress(archive);
        const auto t2 = Clock::now();

        if (restored != input) {
            std::cerr << "cpress-bench: ROUND TRIP MISMATCH\n";
            return {};
        }
        const std::uint32_t crc = cpress::crc32(archive);
        if (run == 0) {
            result.archive_size = archive.size();
            result.archive_crc32 = crc;
        } else if (archive.size() != result.archive_size || crc != result.archive_crc32) {
            std::cerr << "cpress-bench: run " << run + 1 << " made a different archive\n";
            return {};
        }
        result.compress = std::min(result.compress, t1 - t0);
        result.decompress = std::min(result.decompress, t2 - t1);
    }
    return result;
}

constexpr int kNameWidth = 28;

void print_table_header() {
    std::cout << std::left << std::setw(kNameWidth) << "file" << std::right << std::setw(14)
              << "original" << std::setw(14) << "compressed" << std::setw(10) << "crc32"
              << std::setw(10) << "bits/B" << std::setw(12) << "comp ns/B" << std::setw(12)
              << "dec ns/B" << std::setw(11) << "peak MiB" << std::setw(11) << "large MiB"
              << "\n";
}

// Formats a CRC-32 as eight hexadecimal digits.
std::string hex32(std::uint32_t value) {
    std::array<char, 9> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    const std::string digits(buffer.data(), end);
    return std::string(8 - digits.size(), '0') + digits;
}

// Child mode: measure one file, print a table row, append a CSV row.
int run_one(const Options& options) {
    const cpress::Codec* codec = cpress::find_codec(std::string_view{options.codec});
    if (codec == nullptr) {
        std::cerr << "cpress-bench: unknown codec '" << options.codec << "'\n";
        return 2;
    }
    if (!csv_accepts_rows(options.csv)) return 1;
    if (options.cpu && !platform::pin_to_cpu(*options.cpu)) {
        std::cerr << "cpress-bench: cannot pin to logical processor " << *options.cpu << "\n";
        return 1;
    }

    const fs::path path = options.one;
    const cpress::Bytes input = cpress::read_file(path);
    const std::uint64_t large_pages_before = large_page_bytes_so_far();
    const std::optional<Measurement> measured = measure(input, *codec, options.repeat);
    if (!measured) {
        std::cerr << "cpress-bench: '" << path.string() << "' failed verification\n";
        return 1;
    }
    const std::size_t archive_size = measured->archive_size;

    const std::string crc = hex32(measured->archive_crc32);
    const double bits_per_byte =
        input.empty() ? 0.0
                      : 8.0 * static_cast<double>(archive_size) / static_cast<double>(input.size());
    const double compress_ns = per_byte(measured->compress, input.size());
    const double decompress_ns = per_byte(measured->decompress, input.size());
    // Each round trip builds a model to compress and then one to decompress,
    // never two at once, so the peak held one model's large pages.
    const std::uint64_t models = 2 * static_cast<std::uint64_t>(options.repeat);
    const std::uint64_t large_page_bytes =
        (large_page_bytes_so_far() - large_pages_before) / models;
    std::uint64_t peak_bytes = platform::peak_memory_bytes();
    if (platform::peak_memory_omits_large_pages()) peak_bytes += large_page_bytes;
    const double peak_mib = mebibytes(peak_bytes);
    const double large_page_mib = mebibytes(large_page_bytes);

    std::cout << std::left << std::setw(kNameWidth) << path.filename().string() << std::right
              << std::setw(14) << input.size() << std::setw(14) << archive_size << std::setw(10)
              << crc << std::fixed << std::setprecision(3) << std::setw(10) << bits_per_byte
              << std::setprecision(1) << std::setw(12) << compress_ns << std::setw(12)
              << decompress_ns << std::setw(11) << peak_mib << std::setw(11) << large_page_mib
              << "\n";

    std::error_code ec;
    const bool needs_header = !fs::exists(options.csv, ec) || fs::file_size(options.csv, ec) == 0;
    std::ofstream csv(options.csv, std::ios::app);
    if (!csv) {
        std::cerr << "cpress-bench: cannot append to '" << options.csv << "'\n";
        return 1;
    }
    if (needs_header) csv << kCsvHeader << "\n";
    csv << utc_timestamp() << ',' << options.commit << ',' << codec->name << ','
        << csv_quote(path.generic_string()) << ',' << input.size() << ',' << archive_size << ','
        << crc << ',' << std::fixed << std::setprecision(4) << bits_per_byte << ','
        << std::setprecision(2) << compress_ns << ',' << decompress_ns << ',' << options.repeat
        << ',' << peak_mib << ',' << large_page_mib << "\n";
    return 0;
}

// The arguments that make a child measure `file` as this run would.
std::vector<std::string> child_arguments(const Options& options, const std::string& file,
                                         const std::string& commit) {
    std::vector<std::string> args{
        "--one",    file,        "--codec",  options.codec,
        "--csv",    options.csv, "--repeat", std::to_string(options.repeat),
        "--commit", commit};
    if (options.cpu) {
        args.emplace_back("--cpu");
        args.push_back(std::to_string(*options.cpu));
    }
    return args;
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
        if (!csv_accepts_rows(options.csv)) return 1;

        const std::string commit = git_commit();
        const fs::path self = platform::self_path(argv[0]);
        std::cout << "commit " << commit << ", codec " << options.codec << ", fastest of "
                  << options.repeat << ", ";
        if (options.cpu) {
            std::cout << "pinned to logical processor " << *options.cpu;
        } else {
            std::cout << "not pinned";
        }
        std::cout << ", results -> " << options.csv << "\n";
        print_table_header();

        int failures = 0;
        for (const std::string& file : options.files) {
            std::cout << std::flush;
            const int code = platform::run_process(self, child_arguments(options, file, commit));
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
