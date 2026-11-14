// cpress: streaming. Compression and decompression file to file, for inputs
// too large to hold in memory, and the two interfaces a codec that can
// stream reads and writes through.
//
// The archive is the same bytes either way: compress_file() and compress()
// make identical archives of the same input with the same codec.
//
// A codec that can stream has Codec::encode_stream and decode_stream; one
// that cannot is run on whole buffers, so every codec works through the
// functions here.
#pragma once

#include "cpress/cpress.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace cpress {

// Bytes in order, a block at a time. The virtual call is per block, never
// per byte: a codec asks for about a megabyte at once.
class ByteSource {
public:
    ByteSource() = default;
    // An interface: used through a reference, never copied.
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;
    ByteSource(ByteSource&&) = delete;
    ByteSource& operator=(ByteSource&&) = delete;
    virtual ~ByteSource() = default;

    // Fills as much of `buffer` as it can and returns how much. 0 means the
    // end; a short count does not. Throws Error on failure.
    virtual std::size_t read(std::span<std::uint8_t> buffer) = 0;
};

class ByteSink {
public:
    ByteSink() = default;
    ByteSink(const ByteSink&) = delete;
    ByteSink& operator=(const ByteSink&) = delete;
    ByteSink(ByteSink&&) = delete;
    ByteSink& operator=(ByteSink&&) = delete;
    virtual ~ByteSink() = default;

    // Takes all of `bytes`. Throws Error on failure.
    virtual void write(ByteView bytes) = 0;
};

// Writes the archive of the file `input` to `output`, replacing any
// existing file. The input is read once, a block at a time, if the codec
// can stream, and whole otherwise. Throws Error on failure, and then leaves
// no temporary file and any file that was at `output` as it was: the
// archive is written under a temporary name beside `output` and renamed
// once it is complete.
void compress_file(const std::filesystem::path& input, const std::filesystem::path& output,
                   const Codec& codec = default_codec());

// Writes the original data of the file `archive` to `output`, replacing any
// existing file. The archive is held in memory; the output is written a
// block at a time if the codec can stream. Throws Error if the archive is
// malformed or the decoded data fails the size or checksum check, and then
// leaves no output file and no temporary file: the data is written under a
// temporary name beside `output` and renamed only once it has been checked.
void decompress_file(const std::filesystem::path& archive, const std::filesystem::path& output);

// Writes the original data of `archive` to `output`. Throws Error as
// decompress() does. The size and the checksum can only be checked at the
// end, so by then `output` has taken everything: a caller that must not
// keep bad data discards what it was given when this throws.
void decompress(ByteView archive, ByteSink& output);

}  // namespace cpress
