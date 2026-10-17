// Nasdaq TotalView-ITCH 5.0: byte-order helpers, the message catalogue, and decoders
// for the seven order-lifecycle messages (A, F, E, C, X, D, U).
//
// Every message starts with an 11-byte header (type, stock locate, tracking number,
// timestamp). Fields are packed back to back at fixed offsets with no padding, and all
// integers are big-endian.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace itch {

// Assemble big-endian integers byte by byte so the byte order is explicit. Each byte
// is widened before it is shifted; a bare `p[0] << 24` would promote to a 32-bit int.
inline uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t ReadU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Timestamps are 6 bytes: nanoseconds since midnight, Eastern time.
inline uint64_t ReadU48(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadU16(p)) << 32) | ReadU32(p + 2);
}

inline uint64_t ReadU64(const uint8_t* p) {
    return (static_cast<uint64_t>(ReadU32(p)) << 32) | ReadU32(p + 4);
}

struct MessageSpec {
    char type;
    uint8_t length;
    std::string_view name;
};

// Every message type in the spec, with its fixed length in bytes.
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

// The header that starts every message. Bytes 3-4, Nasdaq's internal tracking number,
// are skipped.
struct Header {
    uint16_t locate;     // per-day instrument index, assigned in the Stock Directory
    uint64_t timestamp;  // nanoseconds since midnight ET
};

// 'A' Add Order, and 'F' Add Order with the attributing firm's MPID.
struct AddOrder {
    Header hd;
    uint64_t ref;
    char side;  // 'B' or 'S'
    uint32_t shares;
    std::array<char, 8> stock;  // ticker, space-padded on the right
    uint32_t price;             // 4 implied decimals: 1072600 is $107.2600
    std::array<char, 4> mpid;   // spaces for an 'A'
};

// 'E' Order Executed: `shares` of order `ref` traded at the order's own price.
struct OrderExecuted {
    Header hd;
    uint64_t ref;
    uint32_t shares;
    uint64_t match;  // day-unique id of this execution, also named by Broken Trade ('B')
};

// 'C' Order Executed with Price: like 'E', but carries its own execution price
// (typically a cross execution). The order's displayed price is unchanged.
struct OrderExecutedWithPrice {
    Header hd;
    uint64_t ref;
    uint32_t shares;
    uint64_t match;
    char printable;  // 'N': reported in a later bulk print instead; exclude from volume
    uint32_t price;
};

// 'X' Order Cancel: `shares` removed from order `ref`; the rest keeps resting.
struct OrderCancel {
    Header hd;
    uint64_t ref;
    uint32_t shares;
};

// 'D' Order Delete: the whole order is gone, whatever remained.
struct OrderDelete {
    Header hd;
    uint64_t ref;
};

// 'U' Order Replace: `old_ref` is removed and `new_ref` takes its place with the new
// shares and price. Side, stock and attribution (MPID) are not repeated; they carry over
// from the old order.
struct OrderReplace {
    Header hd;
    uint64_t old_ref;
    uint64_t new_ref;
    uint32_t shares;
    uint32_t price;
};

// Each decoder expects a complete message, including the type byte at offset 0, whose
// length has already been checked against kExpectedLength.
inline Header DecodeHeader(std::span<const uint8_t> m) {
    return {ReadU16(m.data() + 1), ReadU48(m.data() + 5)};
}

inline AddOrder DecodeAddOrder(std::span<const uint8_t> m) {
    AddOrder a{};
    a.hd = DecodeHeader(m);
    a.ref = ReadU64(m.data() + 11);
    a.side = static_cast<char>(m[19]);
    a.shares = ReadU32(m.data() + 20);
    std::memcpy(a.stock.data(), m.data() + 24, a.stock.size());
    a.price = ReadU32(m.data() + 32);
    if (m[0] == 'F') {
        std::memcpy(a.mpid.data(), m.data() + 36, a.mpid.size());
    } else {
        a.mpid.fill(' ');
    }
    return a;
}

inline OrderExecuted DecodeOrderExecuted(std::span<const uint8_t> m) {
    return {DecodeHeader(m), ReadU64(m.data() + 11), ReadU32(m.data() + 19),
            ReadU64(m.data() + 23)};
}

inline OrderExecutedWithPrice DecodeOrderExecutedWithPrice(std::span<const uint8_t> m) {
    return {DecodeHeader(m),        ReadU64(m.data() + 11),   ReadU32(m.data() + 19),
            ReadU64(m.data() + 23), static_cast<char>(m[31]), ReadU32(m.data() + 32)};
}

inline OrderCancel DecodeOrderCancel(std::span<const uint8_t> m) {
    return {DecodeHeader(m), ReadU64(m.data() + 11), ReadU32(m.data() + 19)};
}

inline OrderDelete DecodeOrderDelete(std::span<const uint8_t> m) {
    return {DecodeHeader(m), ReadU64(m.data() + 11)};
}

inline OrderReplace DecodeOrderReplace(std::span<const uint8_t> m) {
    return {DecodeHeader(m), ReadU64(m.data() + 11), ReadU64(m.data() + 19), ReadU32(m.data() + 27),
            ReadU32(m.data() + 31)};
}

}  // namespace itch
