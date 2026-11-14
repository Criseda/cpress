// cpress-bench: measures size, speed and peak memory for each input file and
// appends one CSV row per file, tagged with the git commit.
//
//   cpress-bench [--codec <name>] [--csv <path>] [--cpu <n>] [--repeat <k>] <file>...
//
// Each file is measured in its own child process (this same executable run
// with --one), so the peak-memory figure belongs to that file alone. The
// figure covers the whole process, including the input and output buffers.
//
// A codec that streams is measured file to file, and never holds the input:
// its benchmark is a day of market data, 8.25 GB. A file that is ITCH
// messages and nothing else also gets its size per message, which is how
// market data compressors are compared.
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
#include "cpress/stream.hpp"
#include "itch/order_table.hpp"
#include "message_file.hpp"
#include "model/table_allocator.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
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
          "A codec that streams (itch) is run file to file: its archive goes to\n"
          "the temporary directory and is removed afterwards.\n"
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
    "bits_per_byte,messages,bytes_per_message,compress_ns_per_byte,decompress_ns_per_byte,"
    "compress_mb_per_s,decompress_mb_per_s,repeat,peak_mem_mib,compress_peak_mem_mib,"
    "large_page_mib,evictions";

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

double per_byte(Clock::duration elapsed, std::uint64_t bytes) {
    if (bytes == 0) return 0.0;
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count();
    return ns / static_cast<double>(bytes);
}

// `value` with `digits` digits after the point.
std::string fixed(double value, int digits) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(digits) << value;
    return text.str();
}

double megabytes_per_second(double ns_per_byte) {
    return ns_per_byte == 0.0 ? 0.0 : 1000.0 / ns_per_byte;
}

// What is known of an input before it is measured: its size and CRC-32,
// and how many ITCH messages it is, if it is those and nothing else.
struct Input {
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
    std::optional<std::uint64_t> messages;
};

// One pass over the file, a block at a time.
Input inspect(const fs::path& path) {
    cpress::bench::MessageFile file(path);
    std::uint64_t messages = 0;
    while (!file.next().empty()) ++messages;

    Input input;
    if (file.at_end() && messages > 0) input.messages = messages;
    file.read_rest();
    input.size = file.bytes_read();
    input.crc32 = file.crc32();
    return input;
}

// The archive's size and CRC-32, the fastest compression and decompression
// of `repeat`, and the process's peak memory after the first compression,
// before anything was decompressed.
struct Measurement {
    std::uint64_t archive_size = 0;
    std::uint32_t archive_crc32 = 0;
    Clock::duration compress = Clock::duration::max();
    Clock::duration decompress = Clock::duration::max();
    std::uint64_t compress_peak_bytes = 0;
};

// Records the archive of the first run, and checks that a later run made
// the same one; says so if it did not.
bool same_archive_as_first(Measurement& result, int run, const cpress::Bytes& archive) {
    const std::uint32_t crc = cpress::crc32(archive);
    if (run == 0) {
        result.archive_size = archive.size();
        result.archive_crc32 = crc;
        return true;
    }
    if (archive.size() == result.archive_size && crc == result.archive_crc32) return true;
    std::cerr << "cpress-bench: run " << run + 1 << " made a different archive\n";
    return false;
}

// Compresses and decompresses the file at `path` `repeat` times in memory,
// keeping the fastest time of each. Every round trip must restore the input
// and every archive must have the first one's size and CRC-32; otherwise
// returns nothing, having said which. Each run's archive is freed before
// the next, so the peak memory of a row does not depend on `repeat`.
std::optional<Measurement> measure_in_memory(const fs::path& path, const cpress::Codec& codec,
                                             int repeat) {
    const cpress::Bytes input = cpress::read_file(path);
    Measurement result;
    for (int run = 0; run < repeat; ++run) {
        const auto t0 = Clock::now();
        const cpress::Bytes archive = cpress::compress(input, codec);
        const auto t1 = Clock::now();
        if (run == 0) result.compress_peak_bytes = platform::peak_memory_bytes();
        const cpress::Bytes restored = cpress::decompress(archive);
        const auto t2 = Clock::now();

        if (restored != input) {
            std::cerr << "cpress-bench: ROUND TRIP MISMATCH\n";
            return {};
        }
        if (!same_archive_as_first(result, run, archive)) return {};
        result.compress = std::min(result.compress, t1 - t0);
        result.decompress = std::min(result.decompress, t2 - t1);
    }
    return result;
}

// A sink that keeps nothing: the size and the CRC-32 of what it was given.
class DiscardingSink final : public cpress::ByteSink {
public:
    void write(cpress::ByteView bytes) override {
        size_ += bytes.size();
        crc32_ = cpress::crc32(bytes, crc32_);
    }

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::uint32_t crc32() const noexcept {
        return crc32_;
    }

private:
    std::uint64_t size_ = 0;
    std::uint32_t crc32_ = 0;
};

// A name in the temporary directory for a file of this process's own,
// which is removed with this object.
class TemporaryFile {
public:
    TemporaryFile()
        : path_(fs::temp_directory_path() /
                ("cpress-bench-" + std::to_string(std::random_device{}()) + ".cpz")) {}
    // Owns a file on disk: not copied, and removing it is the destructor's
    // job.
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    ~TemporaryFile() {
        std::error_code ignored;
        fs::remove(path_, ignored);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

// The same for a codec that streams, so that a file larger than memory can
// be measured: the input is never held, and nor is a restored copy. The
// compression is file to file, and its time includes reading the input and
// writing the archive to the temporary directory. The decompression is of
// the archive in memory, as decompress_file() holds it, into a sink that
// keeps nothing. The round trip is checked by the size and the CRC-32 of
// what came out, against those of `input`.
std::optional<Measurement> measure_streamed(const fs::path& path, const Input& input,
                                            const cpress::Codec& codec, int repeat) {
    const TemporaryFile archive_file;
    Measurement result;
    for (int run = 0; run < repeat; ++run) {
        const auto t0 = Clock::now();
        cpress::compress_file(path, archive_file.path(), codec);
        const auto t1 = Clock::now();
        if (run == 0) result.compress_peak_bytes = platform::peak_memory_bytes();

        const cpress::Bytes archive = cpress::read_file(archive_file.path());
        DiscardingSink restored;
        const auto t2 = Clock::now();
        cpress::decompress(archive, restored);
        const auto t3 = Clock::now();

        if (restored.size() != input.size || restored.crc32() != input.crc32) {
            std::cerr << "cpress-bench: ROUND TRIP MISMATCH\n";
            return {};
        }
        if (!same_archive_as_first(result, run, archive)) return {};
        result.compress = std::min(result.compress, t1 - t0);
        result.decompress = std::min(result.decompress, t3 - t2);
    }
    return result;
}

constexpr int kNameWidth = 28;

void print_table_header() {
    std::cout << std::left << std::setw(kNameWidth) << "file" << std::right << std::setw(14)
              << "original" << std::setw(14) << "compressed" << std::setw(10) << "crc32"
              << std::setw(10) << "bits/B" << std::setw(9) << "B/msg" << std::setw(11)
              << "comp ns/B" << std::setw(10) << "dec ns/B" << std::setw(11) << "comp MB/s"
              << std::setw(10) << "dec MB/s" << std::setw(10) << "peak MiB" << std::setw(11)
              << "large MiB"
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
    const Input input = inspect(path);
    const std::uint64_t large_pages_before = large_page_bytes_so_far();
    const std::uint64_t evictions_before = cpress::itch::evictions_recorded();
    const std::optional<Measurement> measured =
        codec->encode_stream != nullptr ? measure_streamed(path, input, *codec, options.repeat)
                                        : measure_in_memory(path, *codec, options.repeat);
    if (!measured) {
        std::cerr << "cpress-bench: '" << path.string() << "' failed verification\n";
        return 1;
    }
    const std::uint64_t archive_size = measured->archive_size;
    const auto runs = static_cast<std::uint64_t>(options.repeat);

    const std::string crc = hex32(measured->archive_crc32);
    const double bits_per_byte =
        input.size == 0 ? 0.0
                        : 8.0 * static_cast<double>(archive_size) / static_cast<double>(input.size);
    // Both are left empty for a file that is not ITCH messages from end to
    // end.
    std::string messages;
    std::string bytes_per_message;
    std::string bytes_per_message_shown = "-";
    if (input.messages) {
        const double ratio =
            static_cast<double>(archive_size) / static_cast<double>(*input.messages);
        messages = std::to_string(*input.messages);
        bytes_per_message = fixed(ratio, 4);
        bytes_per_message_shown = fixed(ratio, 3);
    }
    const double compress_ns = per_byte(measured->compress, input.size);
    const double decompress_ns = per_byte(measured->decompress, input.size);
    const double compress_mb = megabytes_per_second(compress_ns);
    const double decompress_mb = megabytes_per_second(decompress_ns);
    // Each round trip builds a model to compress and then one to decompress,
    // never two at once, so the peak held one model's large pages.
    const std::uint64_t large_page_bytes =
        (large_page_bytes_so_far() - large_pages_before) / (2 * runs);
    std::uint64_t peak_bytes = platform::peak_memory_bytes();
    std::uint64_t compress_peak_bytes = measured->compress_peak_bytes;
    if (platform::peak_memory_omits_large_pages()) {
        peak_bytes += large_page_bytes;
        compress_peak_bytes += large_page_bytes;
    }
    const double peak_mib = mebibytes(peak_bytes);
    const double compress_peak_mib = mebibytes(compress_peak_bytes);
    const double large_page_mib = mebibytes(large_page_bytes);
    // Every compression evicts the same orders; this is one compression's.
    // The count is the process's, so only what these runs added is theirs.
    const std::uint64_t evictions = (cpress::itch::evictions_recorded() - evictions_before) / runs;

    std::cout << std::left << std::setw(kNameWidth) << path.filename().string() << std::right
              << std::setw(14) << input.size << std::setw(14) << archive_size << std::setw(10)
              << crc << std::fixed << std::setprecision(3) << std::setw(10) << bits_per_byte
              << std::setw(9) << bytes_per_message_shown << std::setprecision(1) << std::setw(11)
              << compress_ns << std::setw(10) << decompress_ns << std::setprecision(2)
              << std::setw(11) << compress_mb << std::setw(10) << decompress_mb
              << std::setprecision(1) << std::setw(10) << peak_mib << std::setw(11)
              << large_page_mib << "\n";

    std::error_code ec;
    const bool needs_header = !fs::exists(options.csv, ec) || fs::file_size(options.csv, ec) == 0;
    std::ofstream csv(options.csv, std::ios::app);
    if (!csv) {
        std::cerr << "cpress-bench: cannot append to '" << options.csv << "'\n";
        return 1;
    }
    if (needs_header) csv << kCsvHeader << "\n";
    csv << utc_timestamp() << ',' << options.commit << ',' << codec->name << ','
        << csv_quote(path.generic_string()) << ',' << input.size << ',' << archive_size << ','
        << crc << ',' << std::fixed << std::setprecision(4) << bits_per_byte << ',' << messages
        << ',' << bytes_per_message << ',' << std::setprecision(2) << compress_ns << ','
        << decompress_ns << ',' << std::setprecision(3) << compress_mb << ',' << decompress_mb
        << ',' << std::setprecision(2) << options.repeat << ',' << peak_mib << ','
        << compress_peak_mib << ',' << large_page_mib << ',' << evictions << "\n";
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
