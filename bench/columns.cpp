// cpress-columns: splits a file of ITCH 5.0 messages into columns, one field
// of one message type each, for the fair baseline of stage 5: what a
// careful engineer does without writing a model. bench/baselines.sh
// compresses every column with zstd and adds up the best of each.
//
//   cpress-columns --count FILE          each type's messages and bytes
//   cpress-columns --list FILE           the names of FILE's columns
//   cpress-columns --column NAME FILE    one column, to standard output
//   cpress-columns --head N FILE OUT     the first N messages of FILE, to OUT
//
// A column's name is its source and its encoding:
//
//   type.raw          the type of every message, a byte each
//   T.field.raw       a field of the messages of type T, as its bytes
//   T.field.delta     the field's difference from its value in the previous
//                     message of the type: same width, big-endian, wrapping
//   T.field.dvar      the same difference, zigzagged, as a LEB128 varint
//   all.FIELD.*       one of the three fields every type starts with
//                     (stock_locate, tracking_number, timestamp), across
//                     all types in file order, in the three encodings
//
// Text fields have only the raw encoding. The length prefixes are in no
// column: the type implies them. A file with anything in it that is not an
// ITCH 5.0 message is refused.
//
// A column goes to standard output, never to a file: the columns of a day
// are three times its 8.25 GB, and a pipe into the compressor needs no disk
// at all. The cost is a pass over the file for each column.
#include "cpress/cpress.hpp"
#include "itch/residual.hpp"
#include "itch/schema.hpp"
#include "message_file.hpp"
#include "platform.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace itch = cpress::itch;
using cpress::ByteView;
using cpress::bench::MessageFile;

void print_usage(std::ostream& os) {
    os << "usage: cpress-columns --count FILE\n"
          "       cpress-columns --list FILE\n"
          "       cpress-columns --column NAME FILE\n"
          "       cpress-columns --head N FILE OUT\n"
          "\n"
          "Splits a file of ITCH 5.0 messages into columns.\n"
          "\n"
          "  --count   print each message type's count and bytes\n"
          "  --list    print the names of the file's columns, one a line\n"
          "  --column  write one column to standard output; NAME is one of --list's:\n"
          "            type.raw, T.field.raw, T.field.delta, T.field.dvar or all.FIELD.*\n"
          "  --head    write the first N messages of FILE to OUT\n";
}

enum class Encoding : std::uint8_t { Raw, Delta, Varint };

struct EncodingName {
    std::string_view name;
    Encoding encoding;
};

constexpr std::array<EncodingName, 3> kEncodings{{
    {"raw", Encoding::Raw},
    {"delta", Encoding::Delta},
    {"dvar", Encoding::Varint},
}};

constexpr std::string_view kTypeColumn = "type.raw";
constexpr std::string_view kEveryType = "all";

// The fields every type starts with: those of any layout.
constexpr std::span<const itch::Field> kHeaderFields =
    itch::kLayouts.front().fields.first(itch::kHeaderFields);

// Which bytes a column takes, of which messages, and how it writes them.
struct Column {
    char type = 0;           // of the messages it takes; 0 for every type
    std::size_t offset = 0;  // of the bytes, from the type byte
    std::size_t length = 1;
    Encoding encoding = Encoding::Raw;
};

// Text has no differences worth taking.
bool has_differences(const itch::Field& field) {
    return field.kind != itch::FieldKind::Alpha;
}

// The column called `name`, or nothing if there is no such column.
std::optional<Column> column_named(std::string_view name) {
    if (name == kTypeColumn) return Column{};

    const std::size_t first_dot = name.find('.');
    const std::size_t last_dot = name.rfind('.');
    if (first_dot == std::string_view::npos || first_dot == last_dot) return {};
    const std::string_view source = name.substr(0, first_dot);
    const std::string_view field_name = name.substr(first_dot + 1, last_dot - first_dot - 1);
    const std::string_view encoding_name = name.substr(last_dot + 1);

    const auto encoding = std::ranges::find(kEncodings, encoding_name, &EncodingName::name);
    if (encoding == kEncodings.end()) return {};

    char type = 0;
    std::span<const itch::Field> fields = kHeaderFields;
    if (source != kEveryType) {
        if (source.size() != 1) return {};
        const itch::MessageLayout* layout =
            itch::layout_of(static_cast<std::uint8_t>(source.front()));
        if (layout == nullptr) return {};
        type = layout->type;
        fields = layout->fields;
    }
    const auto field = std::ranges::find(fields, field_name, &itch::Field::name);
    if (field == fields.end()) return {};
    if (encoding->encoding != Encoding::Raw && !has_differences(*field)) return {};
    return Column{.type = type,
                  .offset = field->offset,
                  .length = field->length,
                  .encoding = encoding->encoding};
}

// Standard output through one buffer. Throws cpress::Error if a write
// fails, as when the reader of a pipe has gone.
class Output {
public:
    Output() {
        platform::make_stdout_binary();
        buffer_.reserve(kBufferBytes + kSlackBytes);
    }

    void write(ByteView bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        if (buffer_.size() >= kBufferBytes) flush();
    }

    void flush() {
        if (std::fwrite(buffer_.data(), 1, buffer_.size(), stdout) != buffer_.size() ||
            std::fflush(stdout) != 0) {
            throw cpress::Error("cannot write to standard output");
        }
        buffer_.clear();
    }

private:
    static constexpr std::size_t kBufferBytes = std::size_t{1} << 20;
    static constexpr std::size_t kSlackBytes = 64;

    cpress::Bytes buffer_;
};

// Throws unless `file` was messages from its first byte to its last.
void require_all_messages(const MessageFile& file, const fs::path& path) {
    if (!file.at_end()) {
        throw cpress::Error("'" + path.string() + "' is not ITCH 5.0 messages from end to end");
    }
}

// The most bytes a LEB128 varint of 64 bits takes: seven bits a byte.
constexpr std::size_t kMaxVarintBytes = 10;

// Appends `value` as a LEB128 varint: seven bits a byte, the low ones
// first, the top bit of a byte saying that more follow.
void write_varint(Output& output, std::uint64_t value) {
    std::array<std::uint8_t, kMaxVarintBytes> bytes{};
    std::size_t count = 0;
    while (value >= 0x80) {
        bytes.at(count++) = static_cast<std::uint8_t>(value | 0x80);
        value >>= 7;
    }
    bytes.at(count++) = static_cast<std::uint8_t>(value);
    output.write(std::span(bytes).first(count));
}

int write_column(std::string_view name, const fs::path& path) {
    const std::optional<Column> column = column_named(name);
    if (!column) {
        std::cerr << "cpress-columns: no column called '" << name << "'\n";
        return 2;
    }

    MessageFile file(path);
    Output output;
    const int width = static_cast<int>(column->length);
    std::array<std::uint8_t, sizeof(std::uint64_t)> difference{};
    std::uint64_t previous = 0;  // the column's last value; 0 before the first
    for (ByteView message = file.next(); !message.empty(); message = file.next()) {
        const ByteView body = message.subspan(itch::kLengthPrefixBytes);
        if (column->type != 0 && body.front() != static_cast<std::uint8_t>(column->type)) continue;
        const ByteView bytes = body.subspan(column->offset, column->length);
        if (column->encoding == Encoding::Raw) {
            output.write(bytes);
            continue;
        }
        const std::uint64_t value = itch::load_big_endian(bytes);
        if (column->encoding == Encoding::Delta) {
            const std::span<std::uint8_t> field = std::span(difference).first(column->length);
            itch::store_big_endian(field, itch::wrapping_residual(value, previous, width));
            output.write(field);
        } else {
            write_varint(output, itch::signed_residual(value, previous, width));
        }
        previous = value;
    }
    require_all_messages(file, path);
    output.flush();
    return 0;
}

// How many messages of each type `path` holds, indexed as itch::kLayouts.
using TypeCounts = std::array<std::uint64_t, itch::kLayoutCount>;

TypeCounts count_types(const fs::path& path) {
    MessageFile file(path);
    TypeCounts counts{};
    for (ByteView message = file.next(); !message.empty(); message = file.next()) {
        const itch::MessageLayout* layout = itch::layout_of(message[itch::kLengthPrefixBytes]);
        ++counts.at(itch::index_of(*layout));
    }
    require_all_messages(file, path);
    return counts;
}

// The bytes `count` messages of a type take in a file, prefixes included.
std::uint64_t framed_bytes(const itch::MessageLayout& layout, std::uint64_t count) {
    return count * (itch::kLengthPrefixBytes + layout.length);
}

int print_counts(const fs::path& path) {
    const TypeCounts counts = count_types(path);
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < counts.size(); ++i) {
        messages += counts.at(i);
        bytes += framed_bytes(itch::kLayouts.at(i), counts.at(i));
    }

    std::cout << "type" << std::setw(16) << "messages" << std::setw(18) << "bytes" << std::setw(10)
              << "share" << "\n";
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts.at(i) == 0) continue;
        const std::uint64_t type_bytes = framed_bytes(itch::kLayouts.at(i), counts.at(i));
        std::cout << std::left << std::setw(4) << itch::kLayouts.at(i).type << std::right
                  << std::setw(16) << counts.at(i) << std::setw(18) << type_bytes << std::fixed
                  << std::setprecision(2) << std::setw(9)
                  << 100.0 * static_cast<double>(type_bytes) / static_cast<double>(bytes) << "%\n";
    }
    std::cout << std::left << std::setw(4) << "all" << std::right << std::setw(16) << messages
              << std::setw(18) << bytes << "\n";
    return 0;
}

// The names of a field's columns, one a line: every encoding it has.
void print_columns_of(std::string_view source, const itch::Field& field) {
    for (const EncodingName& encoding : kEncodings) {
        if (encoding.encoding != Encoding::Raw && !has_differences(field)) continue;
        std::cout << source << '.' << field.name << '.' << encoding.name << "\n";
    }
}

// The columns of the types the file has: a type with no messages has
// columns too, all empty, and nothing is learned by compressing them.
int print_columns(const fs::path& path) {
    const TypeCounts counts = count_types(path);
    std::cout << kTypeColumn << "\n";
    for (const itch::Field& field : kHeaderFields) print_columns_of(kEveryType, field);
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (counts.at(i) == 0) continue;
        const itch::MessageLayout& layout = itch::kLayouts.at(i);
        for (const itch::Field& field : layout.fields) {
            print_columns_of(std::string_view(&layout.type, 1), field);
        }
    }
    return 0;
}

// Writes the first `count` messages of `path` to `out_path`, or all of them
// if there are fewer.
int write_head(std::uint64_t count, const fs::path& path, const fs::path& out_path) {
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) throw cpress::Error("cannot create '" + out_path.string() + "'");
    try {
        MessageFile file(path);
        std::uint64_t written = 0;
        std::uint64_t bytes = 0;
        while (written < count) {
            const ByteView message = file.next();
            if (message.empty()) break;
            out.write(reinterpret_cast<const char*>(message.data()),
                      static_cast<std::streamsize>(message.size()));
            ++written;
            bytes += message.size();
        }
        if (written < count) require_all_messages(file, path);
        out.flush();
        if (!out) throw cpress::Error("cannot write '" + out_path.string() + "'");
        std::cout << written << " messages, " << bytes << " bytes -> " << out_path.string() << "\n";
        return 0;
    } catch (const cpress::Error&) {
        // Not left behind half written.
        out.close();
        std::error_code ignored;
        fs::remove(out_path, ignored);
        throw;
    }
}

// Parses all of `text` as a count.
std::optional<std::uint64_t> parse_count(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}

// Runs the command the arguments name, or returns nothing if they name
// none.
std::optional<int> run(std::span<const std::string_view> arguments) {
    if (arguments.empty()) return {};
    const std::string_view command = arguments.front();
    if (command == "--count" && arguments.size() == 2) return print_counts(arguments[1]);
    if (command == "--list" && arguments.size() == 2) return print_columns(arguments[1]);
    if (command == "--column" && arguments.size() == 3) {
        return write_column(arguments[1], arguments[2]);
    }
    if (command == "--head" && arguments.size() == 4) {
        const std::optional<std::uint64_t> count = parse_count(arguments[1]);
        if (!count) return {};
        return write_head(*count, arguments[2], arguments[3]);
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    // argc can be 0: the standard does not promise a program name.
    const std::span<char*> all(argv, static_cast<std::size_t>(argc));
    const std::vector<std::string_view> arguments(all.begin() + std::min<std::ptrdiff_t>(argc, 1),
                                                  all.end());
    try {
        const std::optional<int> code = run(arguments);
        if (code) return *code;
        print_usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "cpress-columns: " << e.what() << "\n";
        return 1;
    }
}
