// A buffer as a ByteSource and a vector as a ByteSink: how a codec that
// streams is run on whole buffers, by the registry and by tests.
#pragma once

#include "cpress/cpress.hpp"
#include "cpress/stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpress::codec {

// Gives the bytes of a buffer, which must outlive it.
class SpanSource final : public ByteSource {
public:
    explicit SpanSource(ByteView bytes) noexcept : left_(bytes) {}

    std::size_t read(std::span<std::uint8_t> buffer) override {
        const std::size_t count = std::min(buffer.size(), left_.size());
        std::ranges::copy(left_.first(count), buffer.begin());
        left_ = left_.subspan(count);
        return count;
    }

private:
    ByteView left_;  // what has not been given yet
};

// Appends what it is given to a vector, which must outlive it.
class VectorSink final : public ByteSink {
public:
    explicit VectorSink(Bytes& bytes) noexcept : bytes_(&bytes) {}

    void write(ByteView bytes) override {
        bytes_->insert(bytes_->end(), bytes.begin(), bytes.end());
    }

private:
    Bytes* bytes_;  // not owned
};

}  // namespace cpress::codec
