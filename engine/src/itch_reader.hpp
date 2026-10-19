// Reads an ITCH stream and hands out one message at a time.
//
// The input is a sequence of messages, each preceded by a 2-byte big-endian length. From
// a stream, the reader refills a fixed buffer in chunks and slides any partial message
// at the end of a chunk to the front before refilling, so every message it returns is
// contiguous. From memory (a mapped file), it frames in place and copies nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <span>
#include <vector>

namespace itch {

class ItchReader {
public:
    // `buffer_size` must exceed the longest frame (2 + 65,535 covers any length prefix).
    // Tests pass small values to force frequent refills.
    explicit ItchReader(std::istream& in, size_t buffer_size = 1 << 20)
        : in_(&in), buf_(buffer_size), data_(buf_.data()) {}

    // The whole input is already in memory.
    explicit ItchReader(std::span<const uint8_t> mapped)
        : data_(mapped.data()), end_(mapped.size()) {}

    bool read_error() const { return in_ != nullptr && in_->bad(); }

    // Offset of the next unconsumed byte. After Next() returns an empty span this is
    // where the frame that could not be returned starts.
    uint64_t offset() const { return consumed_ + pos_; }

    // Unconsumed bytes still buffered: 0 at a clean end of input, more if the input
    // ends inside a message or at a zero-length frame (every ITCH message is at least
    // 12 bytes, so a zero length is always corruption).
    size_t leftover() const { return end_ - pos_; }

    // Returns the next message without its length prefix, or an empty span when no
    // complete message can be returned. The span is valid until the next call.
    std::span<const uint8_t> Next() {
        if (!Fill(2)) return {};
        const size_t length = (static_cast<size_t>(data_[pos_]) << 8) | data_[pos_ + 1];
        if (length == 0 || !Fill(2 + length)) return {};
        std::span<const uint8_t> message(data_ + pos_ + 2, length);
        pos_ += 2 + length;
        return message;
    }

private:
    // Makes sure at least `needed` unread bytes are available, refilling from the stream
    // if there is one. Returns false if the input ends first.
    bool Fill(size_t needed) {
        if (end_ - pos_ >= needed) return true;
        if (in_ == nullptr) return false;  // memory-backed: what is there is all there is
        // Slide the unread tail to the front of the buffer, then top the buffer up.
        const size_t tail = end_ - pos_;
        std::memmove(buf_.data(), buf_.data() + pos_, tail);
        consumed_ += pos_;
        pos_ = 0;
        end_ = tail;
        in_->read(reinterpret_cast<char*>(buf_.data() + end_),
                  static_cast<std::streamsize>(buf_.size() - end_));
        end_ += static_cast<size_t>(in_->gcount());
        return end_ - pos_ >= needed;
    }

    std::istream* in_ = nullptr;  // null when memory-backed
    std::vector<uint8_t> buf_;
    const uint8_t* data_;    // buf_.data(), or the mapped input
    size_t pos_ = 0;         // first unread byte of data_
    size_t end_ = 0;         // one past the last valid byte of data_
    uint64_t consumed_ = 0;  // bytes that have already left the buffer
};

}  // namespace itch
