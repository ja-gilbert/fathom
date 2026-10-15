// fathom: rebuilds the Nasdaq order book from a raw TotalView-ITCH 5.0 file.
// Issue #2: frame the file into messages and count them by type.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

struct MessageSpec {
    char type;
    uint8_t length;
    std::string_view name;
};

// Every message type in the TotalView-ITCH 5.0 spec, with its fixed length in bytes.
constexpr std::array<MessageSpec, 23> kSpecs{{
    {'S', 12, "System Event"},
    {'R', 39, "Stock Directory"},
    {'H', 25, "Stock Trading Action"},
    {'Y', 20, "Reg SHO Restriction"},
    {'L', 26, "Market Participant Position"},
    {'V', 35, "MWCB Decline Level"},
    {'W', 12, "MWCB Status"},
    {'K', 28, "IPO Quoting Period Update"},
    {'J', 35, "LULD Auction Collar"},
    {'h', 21, "Operational Halt"},
    {'A', 36, "Add Order"},
    {'F', 40, "Add Order with MPID"},
    {'E', 31, "Order Executed"},
    {'C', 36, "Order Executed with Price"},
    {'X', 23, "Order Cancel"},
    {'D', 19, "Order Delete"},
    {'U', 35, "Order Replace"},
    {'P', 44, "Trade (Non-Cross)"},
    {'Q', 40, "Cross Trade"},
    {'B', 19, "Broken Trade"},
    {'I', 50, "Net Order Imbalance Indicator"},
    {'N', 20, "Retail Price Improvement Indicator"},
    {'O', 48, "Direct Listing with Capital Raise"},
}};

// Expected length indexed by type byte. 0 means "not a message type".
constexpr std::array<uint8_t, 256> kExpectedLength = [] {
    std::array<uint8_t, 256> table{};
    for (const auto& spec : kSpecs) {
        table[static_cast<uint8_t>(spec.type)] = spec.length;
    }
    return table;
}();

// Reads an ITCH file through a fixed-size buffer and hands out one message at a time.
class ItchReader {
public:
    explicit ItchReader(const char* path) : file_(path, std::ios::binary) {}

    bool is_open() const { return file_.is_open(); }
    bool read_error() const { return file_.bad(); }

    // Byte offset in the file of the next unread byte.
    uint64_t offset() const { return consumed_ + pos_; }

    // Bytes still in the buffer that did not form a complete message.
    size_t leftover() const { return end_ - pos_; }

    // Returns the next message without its 2-byte length prefix, or an empty span
    // when no complete message remains.
    std::span<const uint8_t> Next() {
        if (!Fill(2)) return {};
        const size_t length = (static_cast<size_t>(buf_[pos_]) << 8) | buf_[pos_ + 1];
        if (!Fill(2 + length)) return {};
        std::span<const uint8_t> message(buf_.data() + pos_ + 2, length);
        pos_ += 2 + length;
        return message;
    }

private:
    // Makes sure at least `needed` unread bytes are in the buffer, refilling from the
    // file if necessary. Returns false if the file ends first.
    bool Fill(size_t needed) {
        if (end_ - pos_ >= needed) return true;
        // Slide the unread tail to the front of the buffer, then top the buffer up.
        const size_t tail = end_ - pos_;
        std::memmove(buf_.data(), buf_.data() + pos_, tail);
        consumed_ += pos_;
        pos_ = 0;
        end_ = tail;
        file_.read(reinterpret_cast<char*>(buf_.data() + end_),
                   static_cast<std::streamsize>(buf_.size() - end_));
        end_ += static_cast<size_t>(file_.gcount());
        return end_ - pos_ >= needed;
    }

    static constexpr size_t kBufferSize = 1 << 20;  // 1 MiB

    std::ifstream file_;
    std::vector<uint8_t> buf_ = std::vector<uint8_t>(kBufferSize);
    size_t pos_ = 0;         // first unread byte in buf_
    size_t end_ = 0;         // one past the last valid byte in buf_
    uint64_t consumed_ = 0;  // file bytes that have already left the buffer
};

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: fathom <itch file>\n";
        return 1;
    }
    ItchReader reader(argv[1]);
    if (!reader.is_open()) {
        std::cerr << "could not open " << argv[1] << '\n';
        return 1;
    }

    std::array<uint64_t, 256> counts{};
    uint64_t total = 0;
    const auto start = std::chrono::steady_clock::now();

    while (true) {
        const uint64_t offset = reader.offset();
        const auto msg = reader.Next();
        if (msg.empty()) break;

        const uint8_t type = msg[0];
        const size_t expected = kExpectedLength[type];
        if (expected == 0) {
            std::cerr << "unknown message type '" << static_cast<char>(type) << "' at byte offset "
                      << offset << '\n';
            return 1;
        }
        if (msg.size() != expected) {
            std::cerr << "message type '" << static_cast<char>(type) << "' at byte offset "
                      << offset << " has length " << msg.size() << ", expected " << expected
                      << '\n';
            return 1;
        }
        if (total == 0 && (type != 'S' || msg[11] != 'O')) {
            std::cerr << "file does not start with a System Event 'O' (start of messages)\n";
            return 1;
        }
        ++counts[type];
        ++total;
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (reader.read_error()) {
        std::cerr << "read error at byte offset " << reader.offset() << '\n';
        return 1;
    }
    if (reader.leftover() != 0) {
        std::cerr << reader.leftover() << " leftover bytes at end of file (offset "
                  << reader.offset() << ")\n";
        return 1;
    }

    for (const auto& spec : kSpecs) {
        const uint64_t n = counts[static_cast<uint8_t>(spec.type)];
        if (n == 0) continue;
        std::cout << spec.type << "  " << std::left << std::setw(36) << spec.name << std::right
                  << std::setw(12) << n << '\n';
    }
    const double bytes = static_cast<double>(reader.offset());
    std::cout << "\ntotal " << total << " messages, " << std::fixed << std::setprecision(2)
              << bytes / 1e9 << " GB in " << seconds << " s ("
              << static_cast<double>(total) / seconds / 1e6 << " M msg/s, " << bytes / seconds / 1e6
              << " MB/s)\n";
    return 0;
}
