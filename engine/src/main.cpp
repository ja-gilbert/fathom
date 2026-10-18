// fathom: rebuilds the Nasdaq order book from a raw TotalView-ITCH 5.0 file.
//
//   fathom <itch file>                              replay the whole day and report
//   fathom <itch file> --symbol MSFT --at 10:30:00  print one symbol's book at a time (ET)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string_view>

#include "replay.hpp"

namespace {

int Usage() {
    std::cerr << "usage: fathom <itch file> [--symbol SYM --at HH:MM:SS[.fraction]]\n";
    return 1;
}

// "10:30:00" or "10:30:00.5" (ET) to nanoseconds since midnight.
bool ParseClock(const char* text, uint64_t& ns) {
    unsigned h = 0, m = 0, s = 0;
    int used = 0;
    if (std::sscanf(text, "%u:%u:%u%n", &h, &m, &s, &used) != 3) return false;
    const double fraction = text[used] == '.' ? std::atof(text + used) : 0.0;
    ns = (h * 3600ull + m * 60ull + s) * fathom::kNanosPerSecond +
         static_cast<uint64_t>(fraction * 1e9);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    fathom::ReplayOptions options;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--symbol" && i + 1 < argc) {
            options.symbol = argv[++i];
        } else if (arg == "--at" && i + 1 < argc) {
            if (!ParseClock(argv[++i], options.at)) return Usage();
        } else if (path == nullptr) {
            path = argv[i];
        } else {
            return Usage();
        }
    }
    // A query needs both a symbol and a time.
    if (path == nullptr || options.symbol.empty() != (options.at == fathom::kNoQuery))
        return Usage();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "could not open " << path << '\n';
        return 1;
    }
    const auto result = fathom::Replay(in, std::cout, options);
    if (result.error.empty()) return 0;
    std::cerr << result.error << '\n';
    return 1;
}
