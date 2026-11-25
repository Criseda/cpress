// Whole-file reading and writing for the CLI, the tests and the benchmark.
#include "cpress/io.hpp"

#include "checked_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>

namespace cpress {

Bytes read_file(const std::filesystem::path& path) {
    const std::uint64_t size = detail::size_of_regular_file(path);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw Error("cannot open '" + path.string() + "'");
    }

    Bytes data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<std::size_t>(in.gcount()) != data.size()) {
        throw Error("short read from '" + path.string() + "'");
    }
    return data;
}

void write_file(const std::filesystem::path& path, ByteView data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw Error("cannot create '" + path.string() + "'");
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) {
        throw Error("write to '" + path.string() + "' failed");
    }
}

}  // namespace cpress
