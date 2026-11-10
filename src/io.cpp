#include "cpress/io.hpp"

#include <fstream>
#include <system_error>

namespace cpress {

Bytes read_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw Error("cannot read '" + path.string() + "': not a regular file");
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw Error("cannot read '" + path.string() + "': " + ec.message());
    }

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
