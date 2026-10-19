// The replay: stream ITCH messages through the reader, check that they form one
// complete trading day, feed every order message to the book, and report. With a
// query, it stops at the requested time and prints one symbol's book instead.
//
// It lives in a header rather than main.cpp so the tests can run it on crafted input
// and check exactly which validation fires.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iomanip>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "book.hpp"
#include "itch.hpp"
#include "itch_reader.hpp"

namespace fathom {

constexpr uint64_t kNanosPerSecond = 1'000'000'000;
constexpr uint64_t kOpen = (9 * 3600 + 30 * 60) * kNanosPerSecond;  // 09:30 ET
constexpr uint64_t kNoon = 12 * 3600 * kNanosPerSecond;
constexpr uint64_t kClose = 16 * 3600 * kNanosPerSecond;
constexpr uint64_t kNoQuery = UINT64_MAX;

struct ReplayOptions {
    std::string symbol;           // the symbol a query or top-of-book log is about
    uint64_t at = kNoQuery;       // query: stop here (ns since midnight ET) and print the book
    std::ostream* bbo = nullptr;  // if set, one CSV line each time the symbol's top of book moves;
                                  // seq is the message's 1-based position in the file
};

// A stretch of continuous trading during which a stock's best bid met or passed its
// best ask. Halts and auctions can do this legitimately, so it is logged, not failed.
struct Crossed {
    std::string symbol;
    uint64_t start;
    uint64_t end;
};

struct ReplayResult {
    std::string error;  // empty on success
    uint64_t total = 0;
    uint64_t bytes = 0;
    double seconds = 0;
    std::array<uint64_t, 256> counts{};
    Book::Stats stats;
    size_t live_at_end = 0;
    bool levels_consistent = true;  // levels rebuilt from the orders at noon and at the close
    std::vector<Crossed> crossed;
    std::optional<Levels> snapshot;  // the queried symbol's book at `at`
};

namespace detail {

// HH:MM:SS.nnnnnnnnn
inline std::string Clock(uint64_t ns) {
    const uint64_t s = ns / kNanosPerSecond;
    return std::format("{:02}:{:02}:{:02}.{:09}", s / 3600, s / 60 % 60, s % 60,
                       ns % kNanosPerSecond);
}

inline std::string Dollars(uint32_t price) {
    return std::format("{}.{:04}", price / 10000, price % 10000);
}

inline std::string Trim(const std::array<char, 8>& s) {
    return std::string(s.data(), std::find(s.begin(), s.end(), ' '));
}

// A price ladder: the best five asks above the best five bids, highest price first.
inline void PrintSnapshot(std::ostream& out, const std::string& symbol, uint64_t at,
                          const Levels& book) {
    out << symbol << " at " << Clock(at) << " ET\n";
    std::vector<std::pair<uint32_t, Level>> asks(book.asks.begin(), book.asks.end());
    asks.resize(std::min<size_t>(asks.size(), 5));
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        out << std::format("  ask {:>12} x {:<8} ({} orders)\n", Dollars(it->first),
                           it->second.shares, it->second.count);
    }
    size_t shown = 0;
    for (auto it = book.bids.rbegin(); it != book.bids.rend() && shown < 5; ++it, ++shown) {
        out << std::format("  bid {:>12} x {:<8} ({} orders)\n", Dollars(it->first),
                           it->second.shares, it->second.count);
    }
    if (book.asks.empty() && book.bids.empty()) out << "  (empty)\n";
}

inline void PrintReport(std::ostream& out, const ReplayResult& r, bool integrity) {
    for (const auto& spec : itch::kSpecs) {
        const uint64_t n = r.counts[static_cast<uint8_t>(spec.type)];
        if (n == 0) continue;
        out << spec.type << "  " << std::left << std::setw(36) << spec.name << std::right
            << std::setw(12) << n << '\n';
    }
    const auto& s = r.stats;
    out << "\norder map\n"
        << "  unknown references   " << std::setw(12) << s.unknown_ref << '\n'
        << "  duplicate adds       " << std::setw(12) << s.duplicate_add << '\n'
        << "  locate mismatches    " << std::setw(12) << s.locate_mismatch << '\n'
        << "  over-executions      " << std::setw(12) << s.over_execute << '\n'
        << "  over-cancels         " << std::setw(12) << s.over_cancel << '\n'
        << "  zero-share adds      " << std::setw(12) << s.zero_share_add << '\n'
        << "  peak live orders     " << std::setw(12) << s.peak_live << '\n'
        << "  live at end of day   " << std::setw(12) << r.live_at_end << '\n'
        << "  levels match orders  " << std::setw(12) << (r.levels_consistent ? "yes" : "NO")
        << '\n'
        << "  integrity            " << std::setw(12) << (integrity ? "OK" : "FAILED") << '\n';
    out << "\ncrossed books during continuous trading (09:30-16:00): " << r.crossed.size() << '\n';
    for (size_t i = 0; i < std::min<size_t>(r.crossed.size(), 10); ++i) {
        const auto& c = r.crossed[i];
        out << "  " << std::left << std::setw(8) << c.symbol << std::right << "  " << Clock(c.start)
            << " to " << Clock(c.end) << '\n';
    }
    const double bytes = static_cast<double>(r.bytes);
    out << "\ntotal " << r.total << " messages, " << std::fixed << std::setprecision(2)
        << bytes / 1e9 << " GB in " << r.seconds << " s ("
        << static_cast<double>(r.total) / r.seconds / 1e6 << " M msg/s, " << bytes / r.seconds / 1e6
        << " MB/s)\n";
}

}  // namespace detail

// Replays `in` and writes the report to `out`. Without a query, the result's error is
// empty only if the input is exactly one trading day (System Event 'O' first, 'C' last,
// nothing after), every frame is well formed, every integrity counter is zero, no order
// is left live at the end, and the levels match the orders. With a query, the replay
// stops at the first message past `options.at`, prints that symbol's book, and checks
// only that the levels match the orders.
inline ReplayResult Replay(std::istream& in, std::ostream& out, const ReplayOptions& options = {}) {
    ReplayResult r;
    auto fail = [&r](std::string message) {
        r.error = std::move(message);
        return r;
    };

    itch::ItchReader reader(in);
    Book book;
    std::vector<std::string> names;       // locate -> symbol, from the Stock Directory
    std::vector<uint64_t> crossed_since;  // locate -> when its book crossed, 0 if it isn't
    std::optional<uint16_t> query_locate;
    bool ended = false;  // have we seen System Event 'C'?
    bool noon_checked = false;
    uint64_t last_ts = 0;
    const auto start = std::chrono::steady_clock::now();

    auto name = [&](uint16_t locate) {
        return locate < names.size() && !names[locate].empty() ? names[locate]
                                                               : std::to_string(locate);
    };
    // The queried symbol's top of book as last written to the --bbo log; 0 means no level.
    struct Top {
        uint32_t bid_px = 0, ask_px = 0;
        uint64_t bid_sz = 0, ask_sz = 0;
        uint32_t bid_ct = 0, ask_ct = 0;
        bool operator==(const Top&) const = default;
    };
    Top last_top;
    if (options.bbo) *options.bbo << "ts,seq,offset,bid_px,bid_sz,bid_ct,ask_px,ask_sz,ask_ct\n";

    // After a stock's book changes: note when its best bid meets or passes its best ask
    // during continuous trading (and when that ends), and log the queried symbol's top
    // of book if it moved.
    auto after_change = [&](uint16_t locate, uint64_t ts, uint64_t offset) {
        const Levels& l = book.levels(locate);
        const bool crossed =
            !l.bids.empty() && !l.asks.empty() && l.bids.rbegin()->first >= l.asks.begin()->first;
        if (locate >= crossed_since.size()) crossed_since.resize(locate + 1, 0);
        uint64_t& since = crossed_since[locate];
        if (crossed && since == 0 && ts >= kOpen && ts < kClose) {
            since = ts;
        } else if (!crossed && since != 0) {
            r.crossed.push_back({name(locate), since, ts});
            since = 0;
        }
        if (options.bbo && query_locate == locate) {
            Top top;
            if (!l.bids.empty()) {
                const auto& [px, level] = *l.bids.rbegin();
                top.bid_px = px, top.bid_sz = level.shares, top.bid_ct = level.count;
            }
            if (!l.asks.empty()) {
                const auto& [px, level] = *l.asks.begin();
                top.ask_px = px, top.ask_sz = level.shares, top.ask_ct = level.count;
            }
            if (!(top == last_top)) {
                *options.bbo << ts << ',' << r.total << ',' << offset << ',' << top.bid_px << ','
                             << top.bid_sz << ',' << top.bid_ct << ',' << top.ask_px << ','
                             << top.ask_sz << ',' << top.ask_ct << '\n';
                last_top = top;
            }
        }
    };

    while (true) {
        const uint64_t offset = reader.offset();
        const auto msg = reader.Next();
        if (msg.empty()) break;
        // Built only on failure: doing it per message would allocate 368M strings.
        const auto at = [offset] { return " at byte offset " + std::to_string(offset); };

        const uint8_t type = msg[0];
        const size_t expected = itch::kExpectedLength[type];
        if (expected == 0) return fail(std::format("unknown message type 0x{:02x}{}", type, at()));
        if (msg.size() != expected) {
            return fail(std::format("message type '{}'{} has length {}, expected {}",
                                    static_cast<char>(type), at(), msg.size(), expected));
        }
        const uint64_t ts = itch::Timestamp(msg);
        if (ts > options.at) break;  // the query time has passed; the book is as of `at`
        if (!noon_checked && ts >= kNoon) {
            r.levels_consistent = book.LevelsConsistent();
            noon_checked = true;
        }
        if (ended) return fail("message" + at() + " follows System Event 'C' (end of messages)");
        const bool starts = type == 'S' && msg[11] == 'O';
        if (r.total == 0 && !starts) {
            return fail("file does not start with System Event 'O' (start of messages)");
        }
        if (r.total != 0 && starts)
            return fail("second System Event 'O' (start of messages)" + at());
        ended = type == 'S' && msg[11] == 'C';
        ++r.counts[type];
        ++r.total;
        last_ts = ts;

        const uint16_t locate = itch::ReadU16(msg.data() + 1);
        switch (type) {
            case 'R': {
                if (locate >= names.size()) names.resize(locate + 1);
                names[locate] = detail::Trim(itch::DecodeStockDirectory(msg).stock);
                if (names[locate] == options.symbol) query_locate = locate;
                break;
            }
            case 'A':
            case 'F':
                book.Add(itch::DecodeAddOrder(msg));
                after_change(locate, ts, offset);
                break;
            case 'E': {
                const auto e = itch::DecodeOrderExecuted(msg);
                book.Execute(e.hd.locate, e.ref, e.shares);
                after_change(locate, ts, offset);
                break;
            }
            case 'C': {
                const auto c = itch::DecodeOrderExecutedWithPrice(msg);
                book.Execute(c.hd.locate, c.ref, c.shares);
                after_change(locate, ts, offset);
                break;
            }
            case 'X':
                book.Cancel(itch::DecodeOrderCancel(msg));
                after_change(locate, ts, offset);
                break;
            case 'D':
                book.Delete(itch::DecodeOrderDelete(msg));
                after_change(locate, ts, offset);
                break;
            case 'U':
                book.Replace(itch::DecodeOrderReplace(msg));
                after_change(locate, ts, offset);
                break;
            default:
                break;  // not an order message
        }
    }

    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    r.bytes = reader.offset();

    if (options.at != kNoQuery) {
        if (!query_locate)
            return fail("symbol " + options.symbol + " is not in the Stock Directory");
        r.snapshot = book.levels(*query_locate);
        r.levels_consistent = book.LevelsConsistent();
        detail::PrintSnapshot(out, options.symbol, options.at, *r.snapshot);
        if (!r.levels_consistent)
            return fail("price levels do not match the orders at the query time");
        return r;
    }

    const std::string at = " at byte offset " + std::to_string(reader.offset());
    if (reader.read_error()) {
        return fail("read error after the last complete message, which ends" + at);
    }
    if (reader.leftover() != 0) {
        return fail("truncated or zero-length frame" + at + " (" +
                    std::to_string(reader.leftover()) + " bytes left)");
    }
    if (!ended) return fail("file does not end with System Event 'C' (end of messages)");

    for (uint16_t locate = 0; locate < crossed_since.size(); ++locate) {
        if (crossed_since[locate] != 0)
            r.crossed.push_back({name(locate), crossed_since[locate], last_ts});
    }
    r.stats = book.stats();
    r.live_at_end = book.live();
    r.levels_consistent = r.levels_consistent && book.LevelsConsistent();
    const bool integrity = r.stats.clean() && r.live_at_end == 0 && r.levels_consistent;
    detail::PrintReport(out, r, integrity);
    if (!integrity) return fail("integrity check failed; see the order map section of the report");
    return r;
}

}  // namespace fathom
