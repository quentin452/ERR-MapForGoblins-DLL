#!/usr/bin/env python3
"""Diff a runtime SetEventFlag observer log against the mod's baked marker flags.

The Thread-7 observer (config debug_event_flags) logs every event flag the game
sets to MapForGoblins_events.log as `flag <id> = <0|1>`. Each baked map marker
carries the flag(s) that hide/reveal it (textDisableFlagId1/2/3) or clear it
(clearedEventFlagId) in src/generated/goblin_map_data.cpp. A flag that the game
SET this session but that the mod has NO marker for is a coverage-gap candidate:
the player did something (collected / killed / triggered) the map doesn't show.

This is an analysis aid for designing the P2 in-DLL classifier — it does not
prove a gap (a set flag may be a quest/system flag, not a collectible), it
surfaces the high-range, collectible-shaped unknowns worth inspecting.

Usage:
  tools/analyze_events.py [--events LOG] [--data goblin_map_data.cpp] [--top N]
"""
import argparse
import os
import re
from collections import Counter

# Default to the deployed log + the committed baked data.
DEFAULT_EVENTS = os.path.expanduser(
    "~/Games/ERRv2.2.9.6/dll/offline/logs/MapForGoblins_events.log")
DEFAULT_DATA = os.path.join(
    os.path.dirname(__file__), "..", "src", "generated", "goblin_map_data.cpp")

EVENT_RE = re.compile(r"flag\s+(\d+)\s*=\s*(\d+)")
FLAG_FIELDS = ("textDisableFlagId1", "textDisableFlagId2", "textDisableFlagId3",
               "clearedEventFlagId")
DATA_RE = re.compile(r"\.(%s)\s*=\s*(\d+)" % "|".join(FLAG_FIELDS))

# Map-instance event flags (collectibles, treasure, enemy-defeat) are the big
# 10-digit ids. Below this are mostly system/menu/quest/tutorial flags.
MAP_INSTANCE_MIN = 1_000_000_000


def decade(n):
    """Group a flag id by its digit length, e.g. 1042617188 -> '10-digit'."""
    return f"{len(str(n))}-digit"


def category(i):
    """Category digit of a 10-digit map-instance flag `10 AA BB C DDD`: the
    thousands digit C encodes the flag TYPE (collectible vs door/cutscene/etc)."""
    return (i // 1000) % 10


def map_block(i, block_drop=4):
    """Map-block prefix: id with its last `block_drop` digits dropped, grouping
    sibling flags that co-locate in the same room/area."""
    return i // (10 ** block_drop)


def is_map_instance(i):
    """Map-instance flags (collectibles/treasure/enemy-defeat) are the big
    10-digit ids; below MAP_INSTANCE_MIN are mostly system/menu/quest flags."""
    return i >= MAP_INSTANCE_MIN


def parse_events(path):
    set_ids, clear_ids, all_ids = set(), set(), set()
    n = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = EVENT_RE.search(line)
            if not m:
                continue
            n += 1
            fid, val = int(m.group(1)), int(m.group(2))
            all_ids.add(fid)
            (set_ids if val == 1 else clear_ids).add(fid)
    return set_ids, clear_ids, all_ids, n


def parse_data(path):
    known = set()
    per_field = Counter()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            for m in DATA_RE.finditer(line):
                fid = int(m.group(2))
                if fid == 0:
                    continue
                known.add(fid)
                per_field[m.group(1)] += 1
    return known, per_field


def histogram(ids, label):
    if not ids:
        return
    by_decade = Counter(decade(i) for i in ids)
    print(f"  {label} by id width:")
    for k in sorted(by_decade, key=lambda s: int(s.split("-")[0])):
        print(f"    {k:>9}: {by_decade[k]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--events", default=DEFAULT_EVENTS)
    ap.add_argument("--data", default=DEFAULT_DATA)
    ap.add_argument("--top", type=int, default=40,
                    help="how many unknown candidates to list")
    ap.add_argument("--block-drop", type=int, default=4,
                    help="trailing digits dropped to form a map-block prefix "
                         "(group siblings). 4 = room/area granularity where mod "
                         "markers and session flags start to co-locate; 6+ groups "
                         "whole map-areas (too coarse). Default 4.")
    args = ap.parse_args()

    if not os.path.isfile(args.events):
        raise SystemExit(f"events log not found: {args.events}")
    if not os.path.isfile(args.data):
        raise SystemExit(f"baked data not found: {args.data}")

    set_ids, clear_ids, all_ids, n_lines = parse_events(args.events)
    known, per_field = parse_data(args.data)

    print("=" * 70)
    print("SETEVENTFLAG OBSERVER  vs  BAKED MARKER FLAGS")
    print("=" * 70)
    print(f"events log : {args.events}")
    print(f"baked data : {os.path.normpath(args.data)}")
    print()
    print(f"flag-set events parsed : {n_lines}")
    print(f"distinct flags set (=1): {len(set_ids)}")
    print(f"distinct flags cleared : {len(clear_ids)}")
    print(f"distinct flags total   : {len(all_ids)}")
    print(f"mod known marker flags : {len(known)}  "
          + " ".join(f"{k}={v}" for k, v in per_field.items()))
    print()

    # Collectibles the mod tracks AND the game set this session.
    tracked = set_ids & known
    print(f"[A] SET flags the mod HAS a marker for : {len(tracked)}")
    print("    (collectibles/triggers the map already covers — picked up/done)")
    histogram(tracked, "tracked")
    print()

    # Unknown candidates: set this session, no marker, in the map-instance range.
    unknown = (set_ids - known)
    unknown_hi = sorted(f for f in unknown if f >= MAP_INSTANCE_MIN)
    unknown_lo = unknown - set(unknown_hi)
    print(f"[B] SET flags with NO marker, total              : {len(unknown)}")
    print(f"    of which map-instance range (>= 1e9)         : {len(unknown_hi)}  <-- gap candidates")
    print(f"    of which low/system range (< 1e9)            : {len(unknown_lo)}  (mostly quest/menu/system)")
    histogram(set(unknown_hi), "gap candidates")
    print()
    print(f"[C] mod marker flags NOT set this session        : {len(known - all_ids)}")
    print("    (markers for things not collected/triggered this run — expected)")
    print()

    # Sibling analysis: a map-instance flag block (id with the last `block-drop`
    # digits dropped) where the mod ALREADY has markers is a collectible-rich
    # area; a SET-but-unmarked flag in that same block is far more likely a
    # missed collectible than a flag in a block the mod knows nothing about.
    div = 10 ** args.block_drop
    known_blocks = {k // div for k in known if k >= MAP_INSTANCE_MIN}
    sibling = [c for c in unknown_hi if c // div in known_blocks]
    orphan = [c for c in unknown_hi if c // div not in known_blocks]
    print(f"[D] map-block sibling analysis (block = id // 1e{args.block_drop})")
    print(f"    gap candidates in a block the mod ALREADY marks : {len(sibling)}  <-- HIGH signal (missed collectible)")
    print(f"    gap candidates in a block with no mod markers   : {len(orphan)}  (likely non-collectible events)")
    print()

    # Show the high-signal blocks: the mod's known flags next to the new unmarked
    # siblings the game set this session.
    known_by_block = {}
    for k in known:
        if k >= MAP_INSTANCE_MIN:
            known_by_block.setdefault(k // div, set()).add(k)
    sib_by_block = {}
    for c in sibling:
        sib_by_block.setdefault(c // div, []).append(c)

    print(f"--- HIGH-signal blocks (mod marks the area, missed these) ---")
    for blk in sorted(sib_by_block):
        miss = sorted(sib_by_block[blk])
        have = sorted(known_by_block.get(blk, ()))
        print(f"  block {blk}{'_' * args.block_drop}: "
              f"mod has {len(have)} marker-flags, game set {len(miss)} UNMARKED")
        print(f"    unmarked: {', '.join(str(m) for m in miss[:12])}"
              + (" ..." if len(miss) > 12 else ""))
        print(f"    mod has : {', '.join(str(h) for h in have[:6])}"
              + (" ..." if len(have) > 6 else ""))

    # Category digit: in the 10-digit map-instance flag `10 AA BB C DDD`, the
    # thousands digit C ((id//1000)%10) encodes the flag TYPE. If the mod's
    # collect flags cluster on one category, a SET flag of a DIFFERENT category
    # is a non-item event (door/cutscene/boss-phase), not a missed collectible —
    # the sharpest filter we have without a controlled capture.
    cat = category
    known_cat = Counter(cat(k) for k in known if k >= MAP_INSTANCE_MIN)
    unk_cat = Counter(cat(c) for c in unknown_hi)
    print()
    print("[E] category digit (id//1000 %10) — mod collect-flags vs unmarked")
    print("    cat | mod-known | unmarked-set")
    for c in sorted(set(known_cat) | set(unk_cat)):
        print(f"     {c}  | {known_cat.get(c,0):>9} | {unk_cat.get(c,0)}")
    item_cats = {c for c, n in known_cat.items()
                 if n >= 0.05 * sum(known_cat.values())}
    item_like = sorted(c for c in unknown_hi
                       if c // div in known_blocks and cat(c) in item_cats)
    print(f"    => mod's collectible categories: {sorted(item_cats)}")
    print(f"    => SHARPEST gap candidates (covered block AND collectible category): "
          f"{len(item_like)}")
    for fid in item_like:
        print(f"       {fid}  (block {fid // div}, cat {cat(fid)})")

    print()
    print(f"--- top {args.top} orphan-block candidates (no mod markers nearby) ---")
    for fid in orphan[:args.top]:
        print(f"  {fid}")
    if len(orphan) > args.top:
        print(f"  ... and {len(orphan) - args.top} more")


if __name__ == "__main__":
    main()
