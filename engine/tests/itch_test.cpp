// Tests for the engine's public behaviour: the decoders, the book's lifecycle and
// price-level rules, the reader's framing across refills, and the replay's verdict,
// query and crossed-book log.
//
// The decoder fixtures are the first message of each type in
// data/raw/01302019.NASDAQ_ITCH50, with expected values decoded separately with
// Python's struct module. Everything else is synthetic, so the tests run without the
// data file.

#include "itch.hpp"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "book.hpp"
#include "itch_reader.hpp"
#include "replay.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

// ----- fixtures -------------------------------------------------------------------

std::vector<uint8_t> FromHex(std::string_view hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(
            static_cast<uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return bytes;
}

std::string_view Str(const char* data, size_t n) { return {data, n}; }

void PutBE(std::vector<uint8_t>& v, size_t off, uint64_t value, size_t n) {
    for (size_t i = 0; i < n; ++i) v[off + i] = static_cast<uint8_t>(value >> (8 * (n - 1 - i)));
}

// Synthetic message bodies, laid out per the spec tables.
std::vector<uint8_t> Body(char type, uint16_t locate) {
    std::vector<uint8_t> b(itch::kExpectedLength[static_cast<uint8_t>(type)]);
    b[0] = static_cast<uint8_t>(type);
    PutBE(b, 1, locate, 2);
    return b;
}
std::vector<uint8_t> SystemEvent(char code) {
    auto b = Body('S', 0);
    b[11] = static_cast<uint8_t>(code);
    return b;
}
std::vector<uint8_t> Directory(uint16_t locate, std::string_view stock) {
    auto b = Body('R', locate);
    for (size_t i = 0; i < 8; ++i)
        b[11 + i] = static_cast<uint8_t>(i < stock.size() ? stock[i] : ' ');
    return b;
}
std::vector<uint8_t> Add(uint16_t locate, uint64_t ref, char side, uint32_t shares,
                         uint32_t price) {
    auto b = Body('A', locate);
    PutBE(b, 11, ref, 8);
    b[19] = static_cast<uint8_t>(side);
    PutBE(b, 20, shares, 4);
    for (size_t i = 24; i < 32; ++i) b[i] = ' ';
    PutBE(b, 32, price, 4);
    return b;
}
std::vector<uint8_t> Executed(uint16_t locate, uint64_t ref, uint32_t shares) {
    auto b = Body('E', locate);
    PutBE(b, 11, ref, 8);
    PutBE(b, 19, shares, 4);
    return b;
}
std::vector<uint8_t> Cancel(uint16_t locate, uint64_t ref, uint32_t shares) {
    auto b = Body('X', locate);
    PutBE(b, 11, ref, 8);
    PutBE(b, 19, shares, 4);
    return b;
}
std::vector<uint8_t> Delete(uint16_t locate, uint64_t ref) {
    auto b = Body('D', locate);
    PutBE(b, 11, ref, 8);
    return b;
}
std::vector<uint8_t> Replace(uint16_t locate, uint64_t old_ref, uint64_t new_ref, uint32_t shares,
                             uint32_t price) {
    auto b = Body('U', locate);
    PutBE(b, 11, old_ref, 8);
    PutBE(b, 19, new_ref, 8);
    PutBE(b, 27, shares, 4);
    PutBE(b, 31, price, 4);
    return b;
}
// The same body stamped with a timestamp (nanoseconds since midnight).
std::vector<uint8_t> At(std::vector<uint8_t> body, uint64_t ts) {
    PutBE(body, 5, ts, 6);
    return body;
}

// A file image: each body behind its 2-byte big-endian length.
std::vector<uint8_t> File(const std::vector<std::vector<uint8_t>>& bodies) {
    std::vector<uint8_t> file;
    for (const auto& b : bodies) {
        file.push_back(static_cast<uint8_t>(b.size() >> 8));
        file.push_back(static_cast<uint8_t>(b.size() & 0xff));
        file.insert(file.end(), b.begin(), b.end());
    }
    return file;
}
// A well-formed trading day around `bodies`.
std::vector<uint8_t> Day(std::vector<std::vector<uint8_t>> bodies) {
    bodies.insert(bodies.begin(), SystemEvent('O'));
    bodies.push_back(At(SystemEvent('C'), fathom::kClose + 1));
    return File(bodies);
}

std::istringstream Stream(const std::vector<uint8_t>& bytes) {
    return std::istringstream(std::string(bytes.begin(), bytes.end()));
}

fathom::ReplayResult ReplayBytes(const std::vector<uint8_t>& bytes,
                                 const fathom::ReplayOptions& options = {}) {
    auto in = Stream(bytes);
    std::ostringstream report;
    return fathom::Replay(in, report, options);
}

fathom::Level L(uint64_t shares, uint32_t count) { return {shares, count}; }

uint32_t SharesOf(const fathom::Book& book, uint64_t ref) {
    const auto* order = book.Find(ref);
    return order != nullptr ? order->shares : UINT32_MAX;
}

// A book holding one order: reference 5, locate 1, 100 shares.
fathom::Book OneOrder() {
    fathom::Book book;
    book.Add(itch::DecodeAddOrder(Add(1, 5, 'B', 100, 1000)));
    return book;
}

// True if `counter` is the only Stats field that was tripped, exactly once.
bool OnlyTripped(const fathom::Book& book, uint64_t counter) {
    const auto& s = book.stats();
    const uint64_t total = s.unknown_ref + s.duplicate_add + s.locate_mismatch + s.over_execute +
                           s.over_cancel + s.zero_share_add;
    return counter == 1 && total == 1 && !s.clean();
}

// ----- decoders on real messages from the file --------------------------------------

void TestDecodeAdd() {
    const auto a = itch::DecodeAddOrder(
        FromHex("4101e400000d18c2eb224b000000000000232d5300000258415247582020202000105dd8"));
    CHECK(a.hd.locate == 484);
    CHECK(a.hd.timestamp == 14400000565835ull);  // 04:00:00.000565835 ET
    CHECK(a.ref == 9005);
    CHECK(a.side == 'S');
    CHECK(a.shares == 600);
    CHECK(Str(a.stock.data(), 8) == "ARGX    ");
    CHECK(a.price == 1072600);  // $107.26
    CHECK(Str(a.mpid.data(), 4) == "    ");
}

void TestDecodeAddWithMpid() {
    const auto a = itch::DecodeAddOrder(FromHex(
        "46220300000d18c449143a000000000000177453000000645a565a5a54202020000249f04c45484d"));
    CHECK(a.ref == 6004);
    CHECK(a.side == 'S');
    CHECK(a.shares == 100);
    CHECK(Str(a.stock.data(), 8) == "ZVZZT   ");
    CHECK(a.price == 150000);
    CHECK(Str(a.mpid.data(), 4) == "LEHM");
}

void TestDecodeExecuted() {
    const auto e = itch::DecodeOrderExecuted(
        FromHex("45217d00020d18c9b2926c00000000000000e000000051000000000000440d"));
    CHECK(e.hd.locate == 8573);
    CHECK(e.hd.timestamp == 14400114299500ull);
    CHECK(e.ref == 224);
    CHECK(e.shares == 81);
    CHECK(e.match == 17421);
}

void TestDecodeExecutedWithPrice() {
    const auto c = itch::DecodeOrderExecutedWithPrice(
        FromHex("431a8500011c8c63dd59fd000000000053119700000064000000000000b9e74e000ad764"));
    CHECK(c.hd.locate == 6789);
    CHECK(c.ref == 5443991);
    CHECK(c.shares == 100);
    CHECK(c.match == 47591);
    CHECK(c.printable == 'N');
    CHECK(c.price == 710500);
}

void TestDecodeCancel() {
    const auto x =
        itch::DecodeOrderCancel(FromHex("58146c00000d18c6a4ce940000000000005b73000001f4"));
    CHECK(x.hd.locate == 5228);
    CHECK(x.ref == 23411);
    CHECK(x.shares == 500);
}

void TestDecodeDelete() {
    const auto d = itch::DecodeOrderDelete(FromHex("44207e00000d18c399a2d900000000000003b8"));
    CHECK(d.hd.locate == 8318);
    CHECK(d.hd.timestamp == 14400012002009ull);
    CHECK(d.ref == 952);
}

void TestDecodeReplace() {
    const auto u = itch::DecodeOrderReplace(
        FromHex("5517c000000d18c4e93eb8000000000000029300000000000038e3000006400005c238"));
    CHECK(u.hd.locate == 6080);
    CHECK(u.old_ref == 659);
    CHECK(u.new_ref == 14563);
    CHECK(u.shares == 1600);
    CHECK(u.price == 377400);
}

// The file's second message: the Stock Directory entry for "A" (Agilent), locate 1.
void TestDecodeStockDirectory() {
    const auto r =
        itch::DecodeStockDirectory(FromHex("5200010000"
                                           "0a4a4cee5599"
                                           "4120202020202020"
                                           "4e20000000644e435a2050"
                                           "4e20314e000000004e"));
    CHECK(r.hd.locate == 1);
    CHECK(Str(r.stock.data(), 8) == "A       ");
}

// The real fixtures only use small values. A 64-bit reference with every byte set and
// a share count above 16 bits catch a decoder or book that silently narrows.
void TestWideValues() {
    constexpr uint64_t kRef = 0x8102030405060708ull;
    constexpr uint64_t kRef2 = 0x8807060504030201ull;
    constexpr uint32_t kShares = 0x81020304u;
    constexpr uint32_t kPrice = 0x7735939cu;  // just under the spec's maximum price

    const auto a = itch::DecodeAddOrder(Add(0xffff, kRef, 'B', kShares, kPrice));
    CHECK(a.hd.locate == 0xffff && a.ref == kRef && a.side == 'B' && a.shares == kShares &&
          a.price == kPrice);
    const auto u = itch::DecodeOrderReplace(Replace(7, kRef, kRef2, kShares, kPrice));
    CHECK(u.old_ref == kRef && u.new_ref == kRef2 && u.shares == kShares && u.price == kPrice);

    fathom::Book book;
    book.Add(itch::DecodeAddOrder(Add(1, kRef, 'B', 100000, 1)));
    book.Add(itch::DecodeAddOrder(Add(1, kRef & 0xffffffffull, 'S', 200, 1)));  // same low 32 bits
    CHECK(book.live() == 2);
    book.Execute(1, kRef, 65536);
    CHECK(SharesOf(book, kRef) == 100000 - 65536);
    CHECK(book.stats().clean());
}

// ----- order table ----------------------------------------------------------------

// Insert, erase and find against a reference map, through enough churn that the table
// grows and probe runs wrap; a wrong backward shift on erase would lose or misfile keys.
void TestOrderTable() {
    fathom::OrderTable table(16);
    std::map<uint64_t, uint32_t> reference;
    uint64_t key = 0x9E3779B97F4A7C15ull;
    bool consistent = true;
    for (int i = 0; i < 20000 && consistent; ++i) {
        key = key * 6364136223846793005ull + 1442695040888963407ull;  // a cheap PRNG
        if (i % 3 != 2 || reference.empty()) {
            consistent &=
                table.Insert(key, static_cast<uint32_t>(i)) == (reference.emplace(key, i).second);
        } else {
            const auto victim =
                std::next(reference.begin(), static_cast<long>(key % reference.size()));
            table.Erase(table.Find(victim->first));
            reference.erase(victim);
        }
        if (i % 997 == 0) {
            for (const auto& [k, v] : reference) {
                const size_t slot = table.Find(k);
                consistent &= slot != fathom::OrderTable::npos && table.value(slot) == v;
            }
            consistent &= table.size() == reference.size();
        }
    }
    CHECK(consistent);
    CHECK(table.Find(12345) == fathom::OrderTable::npos);
    CHECK(!table.Insert(key, 0));  // the last key inserted is a duplicate
}

// ----- book: lifecycle and price levels ---------------------------------------------

void TestLifecycle() {
    fathom::Book book;
    book.Add(itch::DecodeAddOrder(Add(484, 9005, 'S', 600, 1072600)));
    book.Execute(484, 9005, 100);  // E: 600 -> 500
    CHECK(SharesOf(book, 9005) == 500);
    book.Cancel(itch::DecodeOrderCancel(Cancel(484, 9005, 200)));  // X: 500 -> 300
    CHECK(SharesOf(book, 9005) == 300);
    book.Execute(484, 9005, 300);  // E: 300 -> 0, the order dies
    CHECK(book.Find(9005) == nullptr);
    CHECK(book.live() == 0);
    CHECK(book.stats().peak_live == 1);
    CHECK(book.stats().clean());
}

void TestReplace() {
    fathom::Book book;
    book.Add(itch::DecodeAddOrder(Add(6080, 659, 'S', 1000, 377300)));
    book.Replace(itch::DecodeOrderReplace(Replace(6080, 659, 14563, 1600, 377400)));
    CHECK(book.Find(659) == nullptr);  // the old reference is retired
    const auto* order = book.Find(14563);
    CHECK(order != nullptr && order->side == 'S' && order->locate == 6080);      // carried over
    CHECK(order != nullptr && order->shares == 1600 && order->price == 377400);  // updated
    CHECK(book.stats().peak_live == 1);
    book.Replace(itch::DecodeOrderReplace(Replace(6080, 14563, 20000, 0, 377400)));  // to zero
    CHECK(book.live() == 0);
    CHECK(book.stats().clean());
}

// Levels follow the orders: totals and counts move with every message, a level is
// gone once empty, and the best bid/ask are the outermost levels.
void TestLevels() {
    fathom::Book book;
    book.Add(itch::DecodeAddOrder(Add(7, 1, 'B', 100, 1072600)));
    book.Add(itch::DecodeAddOrder(Add(7, 2, 'B', 200, 1072600)));  // same level
    book.Add(itch::DecodeAddOrder(Add(7, 3, 'B', 50, 1072500)));   // worse bid
    book.Add(itch::DecodeAddOrder(Add(7, 4, 'S', 300, 1072800)));
    book.Add(itch::DecodeAddOrder(Add(7, 5, 'S', 10, 1072700)));  // best ask
    const auto& l = book.levels(7);
    CHECK(l.bids.size() == 2 && l.asks.size() == 2);
    CHECK(l.bids.rbegin()->first == 1072600 && l.bids.rbegin()->second == L(300, 2));
    CHECK(l.asks.rbegin()->first == 1072700 && l.asks.rbegin()->second == L(10, 1));

    book.Execute(7, 1, 100);  // order 1 dies; its level keeps order 2
    CHECK(l.bids.rbegin()->second == L(200, 1));
    book.Cancel(itch::DecodeOrderCancel(Cancel(7, 2, 50)));
    CHECK(l.bids.rbegin()->second == L(150, 1));
    book.Replace(itch::DecodeOrderReplace(Replace(7, 5, 6, 10, 1072900)));  // ask moves away
    CHECK(l.asks.rbegin()->first == 1072800);
    CHECK(l.asks.size() == 2 && !l.asks.contains(1072700));
    book.Delete(itch::DecodeOrderDelete(Delete(7, 2)));
    CHECK(l.bids.size() == 1 && l.bids.rbegin()->first == 1072500);
    CHECK(book.LevelsConsistent());
    CHECK(book.levels(999).bids.empty());  // a stock with no orders
}

// Each integrity counter, with the one input that must trip it and nothing else.
void TestIntegrityCounters() {
    auto b = OneOrder();
    b.Execute(1, 9, 10);  // unknown reference
    CHECK(OnlyTripped(b, b.stats().unknown_ref));

    b = OneOrder();
    b.Add(itch::DecodeAddOrder(Add(1, 5, 'B', 100, 1000)));  // duplicate add
    CHECK(OnlyTripped(b, b.stats().duplicate_add));

    b = OneOrder();
    b.Add(itch::DecodeAddOrder(Add(1, 6, 'B', 100, 1000)));
    b.Replace(itch::DecodeOrderReplace(Replace(1, 5, 6, 50, 1000)));  // onto a live reference
    CHECK(OnlyTripped(b, b.stats().duplicate_add));

    b = OneOrder();
    b.Replace(itch::DecodeOrderReplace(Replace(1, 5, 5, 50, 1000)));  // keeps its reference
    CHECK(OnlyTripped(b, b.stats().duplicate_add));

    b = OneOrder();
    b.Execute(2, 5, 10);  // locate mismatch
    CHECK(OnlyTripped(b, b.stats().locate_mismatch));

    b = OneOrder();
    b.Execute(1, 5, 150);  // over-execution
    CHECK(OnlyTripped(b, b.stats().over_execute));

    b = OneOrder();
    b.Cancel(itch::DecodeOrderCancel(Cancel(1, 5, 101)));  // over-cancel
    CHECK(OnlyTripped(b, b.stats().over_cancel));

    b = OneOrder();
    b.Add(itch::DecodeAddOrder(Add(1, 6, 'B', 0, 1000)));  // zero-share add
    CHECK(OnlyTripped(b, b.stats().zero_share_add));
}

// ----- reader ---------------------------------------------------------------------

// 3,000 21-byte frames through a 64-byte buffer: a message straddles a refill every
// few messages, and every one must still come out whole and in order.
void TestReaderAcrossRefills() {
    std::vector<std::vector<uint8_t>> bodies;
    for (uint64_t i = 0; i < 3000; ++i)
        bodies.push_back(Delete(static_cast<uint16_t>(i), i * 1000003ull));
    const auto bytes = File(bodies);
    auto in = Stream(bytes);
    itch::ItchReader reader(in, 64);
    uint64_t seen = 0;
    bool in_order = true;
    for (auto msg = reader.Next(); !msg.empty(); msg = reader.Next(), ++seen) {
        if (msg.size() != 19 || itch::DecodeOrderDelete(msg).ref != seen * 1000003ull)
            in_order = false;
    }
    CHECK(seen == 3000);
    CHECK(in_order);
    CHECK(reader.leftover() == 0);
    CHECK(reader.offset() == bytes.size());
}

// ----- replay end to end ------------------------------------------------------------

void TestReplayDay() {
    const auto day = Day({
        Add(1, 5, 'B', 100, 1000000),
        Executed(1, 5, 40),
        Cancel(1, 5, 10),
        Replace(1, 5, 6, 80, 1000100),
        Delete(1, 6),
    });
    const auto r = ReplayBytes(day);
    CHECK(r.error.empty());
    std::ostringstream report;  // the in-memory path must agree with the stream path
    const auto m = fathom::Replay(std::span<const uint8_t>(day), report);
    CHECK(m.error.empty() && m.total == r.total && m.stats.peak_live == r.stats.peak_live);
    CHECK(r.total == 7);
    CHECK(r.counts['A'] == 1 && r.counts['E'] == 1 && r.counts['X'] == 1 && r.counts['U'] == 1 &&
          r.counts['D'] == 1 && r.counts['S'] == 2);
    CHECK(r.stats.peak_live == 1);
    CHECK(r.live_at_end == 0);
    CHECK(r.levels_consistent);
    CHECK(r.crossed.empty());
}

// Every way a file can be wrong that would otherwise be reported as a clean day.
void TestReplayRejectsBadInput() {
    const auto add = Add(1, 5, 'B', 100, 1000000);
    auto truncated = Day({});
    truncated.resize(truncated.size() - 5);
    auto short_add = add;
    short_add.resize(20);

    struct Case {
        const char* name;
        std::vector<uint8_t> bytes;
        const char* reason;  // must appear in the error
    };
    const Case cases[] = {
        {"empty file", {}, "does not end"},
        {"no start event", File({add, SystemEvent('C')}), "does not start"},
        {"no end event", File({SystemEvent('O'), add}), "does not end"},
        {"second start event", File({SystemEvent('O'), SystemEvent('O'), SystemEvent('C')}),
         "second System Event 'O'"},
        {"message after end event", File({SystemEvent('O'), SystemEvent('C'), SystemEvent('C')}),
         "follows System Event 'C'"},
        {"truncated last message", truncated, "frame at byte offset 14"},
        {"zero-length frame", File({SystemEvent('O'), {}, SystemEvent('C')}),
         "frame at byte offset 14"},
        {"unknown message type", File({SystemEvent('O'), {'Z'}}), "unknown message type 0x5a"},
        {"wrong length for type", File({SystemEvent('O'), short_add}), "expected 36"},
        {"order left live at end", Day({add}), "integrity"},
    };
    for (const auto& c : cases) {
        const auto error = ReplayBytes(c.bytes).error;
        if (error.find(c.reason) == std::string::npos) {
            ++failures;
            std::printf("FAIL %s:%d: %s -> \"%s\"\n", __FILE__, __LINE__, c.name, error.c_str());
        }
    }
}

// A query stops at the requested time: the book reflects every message up to it and
// none after.
void TestReplayQuery() {
    const auto day = Day({
        At(Directory(1, "MSFT"), 1),
        At(Add(1, 5, 'B', 100, 1072600), 10),
        At(Add(1, 6, 'S', 50, 1072700), 20),
        At(Add(1, 7, 'B', 300, 1072800), 30),  // would cross the book, but it is after the query
        At(Delete(1, 7), 40),
        At(Delete(1, 5), 50),
        At(Delete(1, 6), 60),
    });
    const auto r = ReplayBytes(day, {"MSFT", 25});
    CHECK(r.error.empty());
    CHECK(r.snapshot.has_value());
    if (!r.snapshot) return;
    CHECK(r.snapshot->bids.size() == 1 && r.snapshot->bids.rbegin()->first == 1072600);
    CHECK(r.snapshot->bids.rbegin()->second == L(100, 1));
    CHECK(r.snapshot->asks.rbegin()->first == 1072700 &&
          r.snapshot->asks.rbegin()->second == L(50, 1));
    CHECK(ReplayBytes(day, {"AAPL", 25}).error.find("not in the Stock Directory") !=
          std::string::npos);
}

// A crossed book during continuous trading is logged with the symbol and its bounds;
// the same crossing before the open is not.
void TestReplayCrossedBook() {
    const uint64_t t = fathom::kOpen + 1000;
    const auto r = ReplayBytes(Day({
        At(Directory(1, "MSFT"), 1),
        At(Add(1, 5, 'B', 100, 1072600), t),
        At(Add(1, 6, 'S', 50, 1072500), t + 1),  // ask below the bid: crossed
        At(Delete(1, 6), t + 2),                 // uncrossed
        At(Delete(1, 5), t + 3),
        At(Add(2, 8, 'B', 100, 2000), 100),  // pre-open crossing on another stock
        At(Add(2, 9, 'S', 100, 1000), 101),
        At(Delete(2, 8), 102),
        At(Delete(2, 9), 103),
    }));
    CHECK(r.error.empty());
    CHECK(r.crossed.size() == 1);
    if (r.crossed.size() != 1) return;
    CHECK(r.crossed[0].symbol == "MSFT" && r.crossed[0].start == t + 1 &&
          r.crossed[0].end == t + 2);
}

// The --bbo log has one line per change to the symbol's top of book (0 for an empty
// side) and none for changes deeper in the book.
void TestReplayBboLog() {
    std::ostringstream log;
    fathom::ReplayOptions options;
    options.symbol = "MSFT";
    options.bbo = &log;
    const auto r = ReplayBytes(Day({
                                   At(Directory(1, "MSFT"), 1),
                                   At(Add(1, 5, 'B', 100, 1072600), 10),
                                   At(Add(1, 6, 'S', 50, 1072700), 20),
                                   At(Add(1, 7, 'B', 300, 1072500), 30),  // deeper: no line
                                   At(Delete(1, 5), 40),  // best bid falls to 1072500
                                   At(Delete(1, 7), 50),
                                   At(Delete(1, 6), 60),
                               }),
                               options);
    CHECK(r.error.empty());
    CHECK(log.str() ==
          "ts,seq,offset,bid_px,bid_sz,bid_ct,ask_px,ask_sz,ask_ct\n"
          "10,3,55,1072600,100,1,0,0,0\n"
          "20,4,93,1072600,100,1,1072700,50,1\n"
          "40,6,169,1072500,300,1,1072700,50,1\n"
          "50,7,190,0,0,0,1072700,50,1\n"
          "60,8,211,0,0,0,0,0,0\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep FAIL lines if a later check crashes

    TestDecodeAdd();
    TestDecodeAddWithMpid();
    TestDecodeExecuted();
    TestDecodeExecutedWithPrice();
    TestDecodeCancel();
    TestDecodeDelete();
    TestDecodeReplace();
    TestDecodeStockDirectory();
    TestWideValues();
    TestOrderTable();
    TestLifecycle();
    TestReplace();
    TestLevels();
    TestIntegrityCounters();
    TestReaderAcrossRefills();
    TestReplayDay();
    TestReplayRejectsBadInput();
    TestReplayQuery();
    TestReplayCrossedBook();
    TestReplayBboLog();

    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
