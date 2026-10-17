// Tests for the engine's public behaviour: the decoders, the order map's lifecycle
// rules, the reader's framing across refills, and the end-to-end replay verdict.
//
// The decoder fixtures are the first message of each type in
// data/raw/01302019.NASDAQ_ITCH50, with expected values decoded separately with
// Python's struct module. Everything else is synthetic, so the tests run without the
// data file.

#include "itch.hpp"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "itch_reader.hpp"
#include "order_map.hpp"
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
    bodies.push_back(SystemEvent('C'));
    return File(bodies);
}

std::istringstream Stream(const std::vector<uint8_t>& bytes) {
    return std::istringstream(std::string(bytes.begin(), bytes.end()));
}

fathom::ReplayResult ReplayBytes(const std::vector<uint8_t>& bytes) {
    auto in = Stream(bytes);
    std::ostringstream report;
    return fathom::Replay(in, report);
}

uint32_t SharesOf(const fathom::OrderMap& map, uint64_t ref) {
    const auto* order = map.Find(ref);
    return order != nullptr ? order->shares : UINT32_MAX;
}

// A map holding one order: reference 5, locate 1, 100 shares.
fathom::OrderMap OneOrder() {
    fathom::OrderMap map;
    map.Add(itch::DecodeAddOrder(Add(1, 5, 'B', 100, 1000)));
    return map;
}

// True if `counter` is the only Stats field that was tripped, exactly once.
bool OnlyTripped(const fathom::OrderMap& map, uint64_t counter) {
    const auto& s = map.stats();
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

// The real fixtures only use small values. A 64-bit reference with every byte set and
// a share count above 16 bits catch a decoder or map that silently narrows.
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

    fathom::OrderMap map;
    map.Add(itch::DecodeAddOrder(Add(1, kRef, 'B', 100000, 1)));
    map.Add(itch::DecodeAddOrder(Add(1, kRef & 0xffffffffull, 'S', 200, 1)));  // same low 32 bits
    CHECK(map.live() == 2);
    map.Execute(1, kRef, 65536);
    CHECK(SharesOf(map, kRef) == 100000 - 65536);
    CHECK(map.stats().clean());
}

// ----- order map lifecycle rules ----------------------------------------------------

void TestLifecycle() {
    fathom::OrderMap map;
    map.Add(itch::DecodeAddOrder(Add(484, 9005, 'S', 600, 1072600)));
    map.Execute(484, 9005, 100);  // E: 600 -> 500
    CHECK(SharesOf(map, 9005) == 500);
    map.Cancel(itch::DecodeOrderCancel(Cancel(484, 9005, 200)));  // X: 500 -> 300
    CHECK(SharesOf(map, 9005) == 300);
    map.Execute(484, 9005, 300);  // E: 300 -> 0, the order dies
    CHECK(map.Find(9005) == nullptr);
    CHECK(map.live() == 0);
    CHECK(map.stats().peak_live == 1);
    CHECK(map.stats().clean());
}

void TestReplace() {
    fathom::OrderMap map;
    map.Add(itch::DecodeAddOrder(Add(6080, 659, 'S', 1000, 377300)));
    map.Replace(itch::DecodeOrderReplace(Replace(6080, 659, 14563, 1600, 377400)));
    CHECK(map.Find(659) == nullptr);  // the old reference is retired
    const auto* order = map.Find(14563);
    CHECK(order != nullptr && order->side == 'S' && order->locate == 6080);      // carried over
    CHECK(order != nullptr && order->shares == 1600 && order->price == 377400);  // updated
    CHECK(map.stats().peak_live == 1);
    map.Replace(itch::DecodeOrderReplace(Replace(6080, 14563, 20000, 0, 377400)));  // to zero
    CHECK(map.live() == 0);
    CHECK(map.stats().clean());
}

// Each integrity counter, with the one input that must trip it and nothing else.
void TestIntegrityCounters() {
    auto m = OneOrder();
    m.Execute(1, 9, 10);  // unknown reference
    CHECK(OnlyTripped(m, m.stats().unknown_ref));

    m = OneOrder();
    m.Add(itch::DecodeAddOrder(Add(1, 5, 'B', 100, 1000)));  // duplicate add
    CHECK(OnlyTripped(m, m.stats().duplicate_add));

    m = OneOrder();
    m.Add(itch::DecodeAddOrder(Add(1, 6, 'B', 100, 1000)));
    m.Replace(itch::DecodeOrderReplace(Replace(1, 5, 6, 50, 1000)));  // onto a live reference
    CHECK(OnlyTripped(m, m.stats().duplicate_add));

    m = OneOrder();
    m.Replace(itch::DecodeOrderReplace(Replace(1, 5, 5, 50, 1000)));  // keeps its reference
    CHECK(OnlyTripped(m, m.stats().duplicate_add));

    m = OneOrder();
    m.Execute(2, 5, 10);  // locate mismatch
    CHECK(OnlyTripped(m, m.stats().locate_mismatch));

    m = OneOrder();
    m.Execute(1, 5, 150);  // over-execution
    CHECK(OnlyTripped(m, m.stats().over_execute));

    m = OneOrder();
    m.Cancel(itch::DecodeOrderCancel(Cancel(1, 5, 101)));  // over-cancel
    CHECK(OnlyTripped(m, m.stats().over_cancel));

    m = OneOrder();
    m.Add(itch::DecodeAddOrder(Add(1, 6, 'B', 0, 1000)));  // zero-share add
    CHECK(OnlyTripped(m, m.stats().zero_share_add));
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
    const auto r = ReplayBytes(Day({
        Add(1, 5, 'B', 100, 1000000),
        Executed(1, 5, 40),
        Cancel(1, 5, 10),
        Replace(1, 5, 6, 80, 1000100),
        Delete(1, 6),
    }));
    CHECK(r.error.empty());
    CHECK(r.total == 7);
    CHECK(r.counts['A'] == 1 && r.counts['E'] == 1 && r.counts['X'] == 1 && r.counts['U'] == 1 &&
          r.counts['D'] == 1 && r.counts['S'] == 2);
    CHECK(r.stats.peak_live == 1);
    CHECK(r.live_at_end == 0);
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
    TestWideValues();
    TestLifecycle();
    TestReplace();
    TestIntegrityCounters();
    TestReaderAcrossRefills();
    TestReplayDay();
    TestReplayRejectsBadInput();

    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}
