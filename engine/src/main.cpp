// fathom: rebuilds the Nasdaq order book from a raw TotalView-ITCH 5.0 file.

#include <fstream>
#include <iostream>

#include "replay.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: fathom <itch file>\n";
        return 1;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "could not open " << argv[1] << '\n';
        return 1;
    }
    const auto result = fathom::Replay(in, std::cout);
    if (result.error.empty()) return 0;
    std::cerr << result.error << '\n';
    return 1;
}
