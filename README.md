# fathom

Rebuilds Nasdaq's full-depth limit order book from a raw TotalView-ITCH 5.0 day file.

fathom is a C++20 engine that replays every order message Nasdaq published on a trading
day, maintains the order book for every listed symbol, and checks its own work. It replays
a 368 million message day in 71 seconds on one core (5.2 M msg/s, 257 ns median per order
update), and its book matches Databento's independently built MBP-1 book on 100% of the
2.28 million top-of-book records compared.

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Getting started](#getting-started)
  - [Build](#build)
  - [Get a day of data](#get-a-day-of-data)
  - [Run](#run)
- [Usage](#usage)
  - [Full-day replay](#full-day-replay)
  - [Book snapshot](#book-snapshot)
  - [Top-of-book log](#top-of-book-log)
  - [Exit status](#exit-status)
- [Tests](#tests)
- [Verification](#verification)
- [Performance](#performance)
- [Design](#design)
- [Project layout](#project-layout)
- [Contributing](#contributing)
- [References](#references)

## Features

- Frames and length-checks all 23 TotalView-ITCH 5.0 message types, and decodes the
  eight it needs (the seven order messages and the Stock Directory), with no
  dependencies beyond the C++ standard library.
- Maintains every displayed order and every price level (shares and order count) for all
  of Nasdaq's roughly 8,700 symbols at once.
- Prints any symbol's price ladder at any time of day, or streams every change to its best
  bid and ask as CSV.
- Counts every violation of the order lifecycle rules instead of asserting, and ends the
  day with an integrity verdict: zero violations, zero orders left live, price levels
  that agree with the orders.
- Verified against Databento's MBP-1 book for three symbol-days with zero mismatches.
- Benchmark and profile-guided-optimization scripts to reproduce the performance numbers.

## Requirements

- Linux (the engine memory-maps the input; tested on Ubuntu 24.04 under WSL2).
- GCC 13 or newer, for C++20 including `std::format`. Tested with g++ 13.3.
- CMake 3.28 or newer and a build tool such as Ninja.
- The Databento tutorial programs in `tutorials/` are off by default. Configuring with
  `-DFATHOM_BUILD_TUTORIALS=ON` fetches [databento-cpp](https://github.com/databento/databento-cpp)
  v0.68.1 and needs the OpenSSL and zstd development packages
  (`sudo apt install libssl-dev libzstd-dev`). The engine needs none of this.
- About 16 GB of disk per trading day of data (5 GB compressed, 11 to 13 GB decompressed).

To reproduce the Databento comparison you also need Python 3.12, [uv](https://docs.astral.sh/uv/),
and a [Databento](https://databento.com) API key.

## Getting started

### Build

```bash
git clone https://github.com/ja-gilbert/fathom.git
cd fathom
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target fathom itch_test
ctest --test-dir build
```

This builds the `fathom` binary and the test binary, and runs the tests. The engine
binary is always compiled with `-O3 -march=native`, so it is tuned to the machine that
builds it.

### Get a day of data

Nasdaq publishes sample days of the full feed at
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`. The two used throughout this project are
`01302019.NASDAQ_ITCH50.gz` and `01302020.NASDAQ_ITCH50.gz`.

```bash
mkdir -p data/raw
cd data/raw
wget 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz'
gunzip 01302019.NASDAQ_ITCH50.gz
cd ../..
```

`data/` is ignored by git.

### Run

```bash
build/fathom data/raw/01302019.NASDAQ_ITCH50
```

Replays the whole day and prints the report described under [Usage](#usage). The first
run reads the file from disk; later runs, with the file in the page cache, take about
70 seconds for the 2019 day.

## Usage

```
fathom <itch file> [--bench] [--symbol SYM (--at HH:MM:SS[.fraction] | --bbo)]
```

| Argument                     | Meaning                                                                                     |
|------------------------------|---------------------------------------------------------------------------------------------|
| `<itch file>`                | A decompressed `.NASDAQ_ITCH50` day file.                                                   |
| `--symbol SYM`               | The symbol for `--at` or `--bbo`. Required by both, and only valid with one of them.        |
| `--at HH:MM:SS[.fraction]`   | Stop at this time of day (Eastern) and print the symbol's price ladder.                     |
| `--bbo`                      | Write a CSV line to stdout every time the symbol's best bid or ask changes. The report goes to stderr. |
| `--bench`                    | Also time every 1,000th order message and print apply-latency percentiles after the report. |

### Full-day replay

Without a query, fathom replays the day and prints a report: message counts by type, the
order-map counters and the integrity verdict, the crossed-book intervals during regular
trading hours, and the throughput.

```
S  System Event                                   6
R  Stock Directory                             8714
H  Stock Trading Action                        8805
Y  Reg SHO Restriction                         8821
L  Market Participant Position               193769
V  MWCB Decline Level                             1
J  LULD Auction Collar                           62
A  Add Order                              162970455
F  Add Order with MPID                      1725898
E  Order Executed                           8096995
C  Order Executed with Price                 158886
X  Order Cancel                             4669874
D  Order Delete                           158273361
U  Order Replace                           27222746
P  Trade (Non-Cross)                        1326184
Q  Cross Trade                                17430
B  Broken Trade                                 116
I  Net Order Imbalance Indicator            3684511

order map
  unknown references              0
  duplicate adds                  0
  locate mismatches               0
  over-executions                 0
  over-cancels                    0
  zero-share adds                 0
  peak live orders          1742866
  live at end of day              0
  levels match orders           yes
  integrity                      OK

crossed books during continuous trading (09:30-16:00): 6
  DLPN      09:46:52.965973483 to 09:47:08.468037144
  DLPN      09:50:24.282167158 to 09:50:41.837889779
  PHUN      10:09:06.897054319 to 10:09:24.271291450
  PHUN      10:10:19.515135908 to 10:18:58.291451178
  SXTC      10:27:49.517281042 to 10:32:49.517128996
  SXTC      11:44:22.042911117 to 11:49:22.040711203

total 368366634 messages, 11.25 GB in 73.48 s (5.01 M msg/s, 153.05 MB/s)
```

The order-map block is the integrity check. `integrity OK` means every counter is zero,
no orders were left live after Nasdaq's End of Messages event, and the price levels
maintained incrementally equal the levels rebuilt from the orders (checked at noon and at
the close).

### Book snapshot

```bash
build/fathom data/raw/01302019.NASDAQ_ITCH50 --symbol MSFT --at 10:30:00
```

Replays up to the given time and prints the five best asks (highest first) and the five
best bids:

```
MSFT at 10:30:00.000000000 ET
  ask     104.8700 x 219      (3 orders)
  ask     104.8600 x 300      (3 orders)
  ask     104.8500 x 600      (5 orders)
  ask     104.8400 x 400      (4 orders)
  ask     104.8300 x 258      (3 orders)
  bid     104.8200 x 600      (4 orders)
  bid     104.8100 x 750      (6 orders)
  bid     104.8000 x 701      (8 orders)
  bid     104.7900 x 2200     (3 orders)
  bid     104.7800 x 821      (8 orders)
```

### Top-of-book log

```bash
build/fathom data/raw/01302019.NASDAQ_ITCH50 --symbol MSFT --bbo > msft-bbo.csv
```

Writes one line per change to the symbol's best bid or ask:

```
ts,seq,offset,bid_px,bid_sz,bid_ct,ask_px,ask_sz,ask_ct
14400028323023,220239,6223165,800000,1,1,0,0,0
14408675188532,227999,6474621,1034800,100,1,0,0,0
```

| Column               | Meaning                                                              |
|----------------------|----------------------------------------------------------------------|
| `ts`                 | Nanoseconds since midnight Eastern, from the message's timestamp.    |
| `seq`                | 1-based ordinal of the message in the file.                          |
| `offset`             | Byte offset of the message in the file.                              |
| `bid_px`, `ask_px`   | ITCH `Price(4)`: four implied decimals, so `1072600` is $107.26. `0` means the side is empty. |
| `bid_sz`, `ask_sz`   | Displayed shares at the best price.                                  |
| `bid_ct`, `ask_ct`   | Number of orders at the best price.                                  |

### Exit status

`0` if the replay completed and the integrity verdict is OK. `1` for a usage error, an
unreadable or malformed file, or a failed integrity check; the reason is printed to
stderr, with the byte offset of the offending message when there is one.

## Tests

```bash
ctest --test-dir build
```

The tests are built with AddressSanitizer and UndefinedBehaviorSanitizer and cover the
message decoders on real captured bytes, the order table against `std::map` under churn,
the order lifecycle rules, and whole-day replays of small synthetic files, including the
inputs the replay must reject.

## Verification

Two independent checks, both run on two full trading days (2019-01-30 and 2020-01-30).

### Internal consistency

The book counts every violation of the lifecycle rules: a modify naming an unknown
reference, a duplicate add, a stock locate that disagrees with the add, an execution or
cancel for more shares than remained, an add with zero shares. The day passes only if
every counter is zero, no orders are left live after the End of Messages event (a bug
that fails to take shares off an order shows up here and nowhere else), and the price
levels rebuilt from scratch out of the orders equal the levels maintained incrementally.

| Day        | Messages    | Peak live orders | Violations | Live at end | Levels match |
|------------|-------------|------------------|------------|-------------|--------------|
| 2019-01-30 | 368,366,634 | 1,742,866        | 0          | 0           | yes          |
| 2020-01-30 | 423,285,709 | 1,925,638        | 0          | 0           | yes          |

The replay also logs every stretch of regular trading in which a stock's best bid met or
crossed its best ask. The six such intervals on 2019-01-30, all in three small caps, line
up with LULD limit states and trading pauses, when Nasdaq does not uncross the displayed
book.

### Against Databento

Databento builds its own book from the same feed and publishes it as the `XNAS.ITCH`
MBP-1 schema. [`verify_bbo.py`](research/src/research/verify_bbo.py) aligns fathom's
`--bbo` log with Databento's record for the same message (Databento's `sequence` is the
ordinal of the ITCH message, so the alignment is exact, not by timestamp), keeps the last
end-of-event record per message, and compares bid and ask price, size and order count
during 09:30 to 16:00 ET.

| Symbol | Day        | Records compared | Mismatches |
|--------|------------|------------------|------------|
| MSFT   | 2019-01-30 | 851,544          | 0          |
| MSFT   | 2020-01-30 | 663,598          | 0          |
| AAPL   | 2020-01-30 | 766,884          | 0          |

To reproduce, export your key as `DATABENTO_API_KEY`, fetch the reference file with the
Databento Python client (check the cost with `client.metadata.get_cost` first; one
symbol-day of MBP-1 is a few MB), and run the comparison:

```python
import databento as db

client = db.Historical()  # reads DATABENTO_API_KEY
client.timeseries.get_range(
    dataset="XNAS.ITCH", schema="mbp-1", symbols="MSFT",
    start="2019-01-30", end="2019-01-31",
).to_file("data/databento/xnas-itch-20190130.mbp-1.MSFT.dbn.zst")
```

```bash
build/fathom data/raw/01302019.NASDAQ_ITCH50 --symbol MSFT --bbo > data/databento/msft-bbo-20190130.csv
cd research
uv run verify-bbo ../data/databento/msft-bbo-20190130.csv \
    ../data/databento/xnas-itch-20190130.mbp-1.MSFT.dbn.zst --date 2019-01-30
```

The script prints the number of records compared and the mismatches, with the first few
mismatches side by side if there are any.

## Performance

Measured on 2019-01-30 (368 M messages, 11.25 GB) on an i9-14900HX under WSL2, pinned to
one P-core, with the file in the page cache, g++ 13.3. Wall time is the median of three
full-day runs. Latency is the time to decode and apply one order message, sampled every
1,000th.

| Step | Change                                                       | Wall time | msg/s  | p50    | p99.9     |
|------|--------------------------------------------------------------|-----------|--------|--------|-----------|
|      | baseline (`-O2`, `std::unordered_map`, `std::map` levels)    | 170.92 s  | 2.16 M | 483 ns | 11,662 ns |
| 0    | `-O3 -march=native`                                          | 158.43 s  | 2.33 M | 447 ns | 5,076 ns  |
| 1    | pre-sized order map                                          | 141.00 s  | 2.61 M | 381 ns | 4,626 ns  |
| 2    | flat hash table + order pool                                 | 122.64 s  | 3.00 M | 336 ns | 4,156 ns  |
| 3    | sorted-vector price levels                                   | 80.73 s   | 4.56 M | 254 ns | 3,574 ns  |
| 4    | replace in place                                             | skipped: only 1.6% of replaces keep their price | | | |
| 5    | memory-mapped input                                          | 77.54 s   | 4.75 M | 258 ns | 3,867 ns  |
| 6    | profile-guided optimization [1]                              | 71.23 s   | 5.17 M | 257 ns | 3,911 ns  |

[1] Profile trained on 2020-01-30 and benchmarked on 2019-01-30. Trained on the
benchmarked day itself: 64.76 s, p50 245 ns, p99.9 3,738 ns, the upper bound of what PGO
gives here.

Every step was re-verified against the MSFT 2019 Databento reference before it was kept.
The biggest single win was the price levels: `std::map` touched a heap node per level on
every update, while a sorted vector with the best price at the back keeps the hot end of
the book in a few cache lines. A flat array indexed by price tick was ruled out because
6,179 of the 8,695 symbols span more than a million ticks in a day.

To reproduce a row:

```bash
scripts/bench.sh data/raw/01302019.NASDAQ_ITCH50          # median of 3 runs on CPU 2, then latency
CORE=4 scripts/bench.sh data/raw/01302019.NASDAQ_ITCH50   # pin to a different CPU
```

And the PGO build, which trains on one day and leaves `build/` configured to use the
profile:

```bash
scripts/pgo.sh data/raw/01302020.NASDAQ_ITCH50
scripts/bench.sh data/raw/01302019.NASDAQ_ITCH50
```

## Design

The engine is header-only under [`engine/src`](engine/src), with a thin
[`main.cpp`](engine/src/main.cpp). Data flows through three layers:

```
ITCH file  --mmap-->  ItchReader  --one frame-->  itch:: decoders  --->  Book
                      (itch_reader.hpp)           (itch.hpp)             (book.hpp)
                                                                           |
                                      +------------------------------------+
                                      |                                    |
                                OrderTable (order_table.hpp)        Levels, one per stock
                                reference -> pool slot              bids / asks: vector of
                                open addressing, no tombstones      (price -> shares, count)
                                                                    sorted worst to best
```

**Parser.** Messages arrive as a 2-byte big-endian length prefix plus body. The reader
frames them in place over the memory-mapped file, so no byte is copied before it is
decoded. The decoders are plain functions over a `std::span` at the spec's fixed offsets:
big-endian 16, 32, 48 and 64 bit reads, 6-byte nanosecond timestamps, `Price(4)` with
four implied decimals. The message types and their fixed lengths are in one table, and a
message of the wrong length is a hard error.

**Order table.** Five of the seven order message types identify an order only by its
reference number, so the engine has to remember what the add said. `OrderTable` is an
open-addressing hash map from 64-bit reference to a 32-bit slot in a flat order pool:
Fibonacci hashing, linear probing, and backward-shift erase so deletes leave no
tombstones. It is pre-sized to four million slots, above the busiest day's 1.93 million
live orders, so neither the table nor the pool grows during the replay. Orders live in
one `std::vector` with a free list; an `Order` is 12 bytes (stock locate, side, shares,
price).

**Book.** The lifecycle rules in one place: `E`, `C` and `X` subtract shares and the order
dies at zero; `D` removes it; `U` retires the old reference and inserts the new one with
the old side and stock, and a replace to zero shares just kills it. Every order change
also adjusts its price level, and a level disappears with its last order. Each side's
levels are a vector sorted worst to best, so the top of book is the last element and the
updates that cluster near the top move only a few entries. Every violation of the rules
is counted rather than asserted, so one run of a real day reports every problem at once.

**Replay.** The day loop: it validates the session (exactly one trading day, with the
start and end events in order), dispatches messages to the book, tracks crossed books
during regular hours, serves the `--at` and `--bbo` queries, and prints the report and
the integrity verdict.

## Project layout

```
.
├── CMakeLists.txt
├── CONTRIBUTING.md
├── engine/
│   ├── src/
│   │   ├── itch.hpp           message layouts and field decoders
│   │   ├── itch_reader.hpp    framing over a mapped file or a stream
│   │   ├── order_table.hpp    open-addressing map: reference -> pool slot
│   │   ├── book.hpp           orders, price levels, lifecycle rules, integrity counters
│   │   ├── replay.hpp         day loop, queries, report
│   │   └── main.cpp           command line
│   └── tests/
│       └── itch_test.cpp
├── research/                  Python package (uv): the verify-bbo comparison
├── scripts/
│   ├── bench.sh               pinned median-of-3 benchmark
│   └── pgo.sh                 profile-guided build
└── tutorials/                 Databento order-book tutorials, separate from the engine
```

## Contributing

It is a solo project, but the conventions in [CONTRIBUTING.md](CONTRIBUTING.md) keep the
history traceable: one branch and one pull request per issue, squash merges, and a clean
build with `-Wall -Wextra -Wpedantic` plus passing tests before merging. Work is tracked in
the repository's issues and milestones.

## References

- Nasdaq, [TotalView-ITCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)
- Nasdaq, [sample ITCH data](https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/)
- Databento, [XNAS.ITCH dataset](https://databento.com/datasets/XNAS.ITCH) and the
  [MBP-1 schema](https://databento.com/docs/schemas-and-data-formats/mbp-1)
