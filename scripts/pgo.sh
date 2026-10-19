#!/usr/bin/env bash
# Profile-guided optimization of the engine binary: build instrumented, train on one
# day, rebuild using the profile. Leaves build/ configured with FATHOM_PGO=use. Train
# on a different day than you benchmark, or the profile has seen its own test.
#
#   scripts/pgo.sh data/raw/01302019.NASDAQ_ITCH50
set -euo pipefail
file=${1:?usage: scripts/pgo.sh <itch file>}
rm -rf build/pgo  # a fresh profile: instrumented runs merge into existing counts
cmake -S . -B build -DFATHOM_PGO=generate > /dev/null
cmake --build build --target fathom
build/fathom "$file" > /dev/null
cmake -S . -B build -DFATHOM_PGO=use > /dev/null
cmake --build build --target fathom
