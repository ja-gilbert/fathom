// The replay: stream ITCH messages through the reader, check that they form one
// complete trading day, feed every order message to the order map, and report.
//
// Lives here in a header rather than main so tests can run it on crafted input
// and check exactly which validation is firing.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>

#include "itch.hpp"
#include "itch_reader.hpp"
#include "order_map.hpp"

namespace fathom {

struct ReplayResult {
    std::string error;  // empty on success
    uint64_t total = 0;
    uint64_t bytes = 0;
    double seconds = 0;
    std::array<uint64_t, 256> counts{};
    OrderMap::Stats stats;
    size_t live_at_end = 0;
};

namespace detail {

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
        << "  integrity            " << std::setw(12) << (integrity ? "OK" : "FAILED") << '\n';
    const double bytes = static_cast<double>(r.bytes);
    out << "\ntotal " << r.total << " messages, " << std::fixed << std::setprecision(2)
        << bytes / 1e9 << " GB in " << r.seconds << " s ("
        << static_cast<double>(r.total) / r.seconds / 1e6 << " M msg/s, " << bytes / r.seconds / 1e6
        << " MB/s)\n";
}

}  // namespace detail

// Replays `in` and writes the report to `out`. The result's error is empty only if the
// input is exactly one trading day, every frame is well formed, every integrity
// counter is zero, and no order is left live at the end
inline ReplayResult Replay(std::istream& in, std::ostream& out) {
    ReplayResult r;
    auto fail = [&r](std::string message) {
        r.error = std::move(message);
        return r;
    };

    itch::ItchReader reader(in);
    OrderMap orders;
    bool ended = false;  // have we seen System Event 'C'?
    const auto start = std::chrono::steady_clock::now();

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

        switch (type) {
            case 'A':
            case 'F':
                orders.Add(itch::DecodeAddOrder(msg));
                break;
            case 'E': {
                const auto e = itch::DecodeOrderExecuted(msg);
                orders.Execute(e.hd.locate, e.ref, e.shares);
                break;
            }
            case 'C': {
                const auto c = itch::DecodeOrderExecutedWithPrice(msg);
                orders.Execute(c.hd.locate, c.ref, c.shares);
                break;
            }
            case 'X':
                orders.Cancel(itch::DecodeOrderCancel(msg));
                break;
            case 'D':
                orders.Delete(itch::DecodeOrderDelete(msg));
                break;
            case 'U':
                orders.Replace(itch::DecodeOrderReplace(msg));
                break;
            default:
                break;  // not an order message
        }
    }

    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    r.bytes = reader.offset();
    const std::string at = " at byte offset " + std::to_string(reader.offset());
    if (reader.read_error()) {
        return fail("read error after the last complete message, which ends" + at);
    }
    if (reader.leftover() != 0) {
        return fail("truncated or zero-length frame" + at + " (" +
                    std::to_string(reader.leftover()) + " bytes left)");
    }
    if (!ended) return fail("file does not end with System Event 'C' (end of messages)");

    r.stats = orders.stats();
    r.live_at_end = orders.live();
    const bool integrity = r.stats.clean() && r.live_at_end == 0;
    detail::PrintReport(out, r, integrity);
    if (!integrity) return fail("integrity check failed; see the order map section of the report");
    return r;
}

}  // namespace fathom
