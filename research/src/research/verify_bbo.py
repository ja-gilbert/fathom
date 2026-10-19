"""Compare fathom's top-of-book log for one symbol with Databento's MBP-1 for the same day.

    fathom <itch file> --symbol MSFT --bbo > msft-bbo.csv
    uv run verify-bbo msft-bbo.csv data/databento/xnas-itch-20190130.mbp-1.MSFT.dbn.zst
"""

import argparse
from datetime import datetime
from zoneinfo import ZoneInfo

import databento as db
import pandas as pd

UNDEF_PRICE = 9223372036854775807  # Databento's "no price"
F_LAST = 128  # flag bit: last record of an event, so the book it carries is complete
NS = 10**9
OPEN = (9 * 3600 + 30 * 60) * NS  # 09:30 ET, ns since midnight
CLOSE = 16 * 3600 * NS
FIELDS = ["bid_px", "bid_sz", "bid_ct", "ask_px", "ask_sz", "ask_ct"]


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("fathom_csv", help="output of fathom --symbol SYM --bbo")
    parser.add_argument("dbn", help="Databento MBP-1 file for the same symbol and day")
    parser.add_argument(
        "--date", default="2019-01-30", help="trading date (default 2019-01-30)"
    )
    parser.add_argument(
        "--show", type=int, default=10, help="mismatches to print (default 10)"
    )
    args = parser.parse_args()

    # ITCH timestamps count from midnight Eastern; Databento's count from the Unix epoch.
    midnight = datetime.fromisoformat(args.date).replace(
        tzinfo=ZoneInfo("America/New_York")
    )
    midnight_ns = int(midnight.timestamp()) * NS

    ours = pd.read_csv(args.fathom_csv)
    ours[["bid_px", "ask_px"]] *= (
        100_000  # Price(4) -> 1e-9 fixed point; 0 (no level) stays 0
    )

    theirs = db.DBNStore.from_file(args.dbn).to_df(price_type="fixed")
    theirs = theirs[theirs["flags"] & F_LAST != 0]
    theirs = theirs.rename(columns={f"{f}_00": f for f in FIELDS})
    theirs["ts_event"] = theirs["ts_event"].astype("int64")
    theirs["sequence"] = theirs["sequence"].astype("int64")  # uint32 in the file
    for side in (
        "bid",
        "ask",
    ):  # an empty side is UNDEF with size and count 0; use 0 like ours
        theirs.loc[theirs[f"{side}_px"] == UNDEF_PRICE, f"{side}_px"] = 0
    # A trade and the cancel it causes share a sequence number and both carry F_LAST; the
    # last record for a sequence is the one with the book after that message.
    theirs = theirs.groupby("sequence").last()[["ts_event", *FIELDS]].reset_index()

    # fathom's book after the same message as each Databento record, during continuous trading.
    merged = pd.merge_asof(
        theirs,
        ours.sort_values("seq"),
        left_on="sequence",
        right_on="seq",
        suffixes=("_db", "_fm"),
    )
    window = (merged["ts_event"] >= midnight_ns + OPEN) & (
        merged["ts_event"] < midnight_ns + CLOSE
    )
    merged = merged[window]

    same = (
        merged[[f + "_db" for f in FIELDS]].to_numpy()
        == merged[[f + "_fm" for f in FIELDS]].to_numpy()
    ).all(axis=1)
    total, matches = len(merged), int(same.sum())
    print(f"end-of-event records compared (09:30-16:00 ET): {total:,}")
    print(
        f"matches {matches:,}, mismatches {total - matches:,} ({100 * matches / total:.4f}% match)"
    )

    bad = merged[~same]
    if len(bad) == 0:
        return
    print(
        f"\nfirst {min(args.show, len(bad))} mismatches (prices in 1e-9 dollars; seq is the message"
    )
    print("number in the file, offset its byte offset):")
    for _, row in bad.head(args.show).iterrows():
        et = pd.Timestamp(int(row["ts_event"]), unit="ns", tz="UTC").tz_convert(
            "America/New_York"
        )
        print(
            f"\n  {et:%H:%M:%S.%f} ET  seq={int(row['sequence'])}  offset={int(row['offset'])}"
        )
        for who in ("db", "fm"):
            vals = "  ".join(f"{f}={int(row[f'{f}_{who}'])}" for f in FIELDS)
            print(f"    {'databento' if who == 'db' else 'fathom   '}  {vals}")


if __name__ == "__main__":
    main()
