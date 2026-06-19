#!/usr/bin/env python3
"""flagdiff — find the PERSISTED event flags that flipped between two ER saves.

The reliable way to get an NPC's "dead / quest-unfinishable" flag for the Quest
Browser's Part 2 grey-out: diff a save where the NPC is ALIVE against one where
it's DEAD, and read which flags went 0->1 *in the save file* (i.e. persisted).
This filters out the TRANSIENT flags the in-game Event-flag hook also logs
(e.g. Varre's 1042365008 / 3082 fired at the kill but never persisted — only
1042369205 did). See memory: quest-browser Part 2.

Usage:
    tools/flagdiff.py <alive.err> <dead.err> [--bst PATH] [--range LO-HI ...]

  <alive.err> / <dead.err>   ER0000.err (or .sl2) saves; the game's active save
                             is ER0000.err for ERR. Make a backup BEFORE the NPC
                             dies (the "alive" side); the live save after = "dead".
  --bst PATH                 eventflag_bst.txt from er-save-lib
                             (default: /tmp/ER-Save-Lib/src/res/eventflag_bst.txt)
  --range LO-HI              only report flag ids in [LO,HI] (repeatable). Handy:
                             --range 1042360000-1042369999 for one NPC's namespace.
  --char N                   character slot (default 0)

Output: the flags newly SET (alive->dead) and newly CLEARED, so you can pick the
NPC's persistent death flag and wire it as NpcQuest::fail_flag.

Layout facts (er-save-lib): char-slot event-flag region in the file, length
0x1BF99F; flag id -> byte via the BST: block=id//1000, region_index=bst[block],
abs = region_off + region_index*125 + (id%1000)//8, bit = 7-((id%1000)%8).
The region_off per active char is found by scanning for the slot; we derive it
the same way er-save-lib does (slot stride 0x280010 from the first slot).
"""
import sys
import argparse
import re
import subprocess

REG_LEN = 0x1BF99F
BLOCK = 125
# The per-character flag-region offset is NOT constant across saves — it shifts
# as earlier data (inventory, etc.) grows (observed 0x3689D vs 0x36C0D for the
# SAME character). The original bug hardcoded it and silently read wrong bytes ->
# garbage flag ids on any save whose layout differed. We now ask er-save-lib's
# errflags for the real offset per file.
DEFAULT_ERRFLAGS = "/tmp/ER-Save-Lib/target/release/errflags"


def load_bst(path):
    inv = {}
    with open(path) as f:
        for line in f:
            b, r = line.split(",")
            inv[int(r)] = int(b)            # region_index -> block
    return inv


def region_off(save_path, errflags, char):
    # Ask errflags for the real per-file offset of `char`'s event-flag region.
    # errflags prints e.g.  "char[0] active name=... eventflags@0x36C0D".
    out = subprocess.run([errflags, "get", save_path, "6001"],
                         capture_output=True, text=True).stdout
    offs = re.findall(r"eventflags@0x([0-9A-Fa-f]+)", out)
    if char >= len(offs):
        sys.exit(f"error: char {char} not active in {save_path} "
                 f"(found {len(offs)} chars). Is errflags at --errflags correct?")
    return int(offs[char], 16)


def decode(inv, rb, k):
    region_index, bb = divmod(rb, BLOCK)
    blk = inv.get(region_index)
    if blk is None:
        return None
    idx = bb * 8 + k
    if idx >= 1000:
        return None
    return blk * 1000 + idx


def read_region(save_path, errflags, char):
    off = region_off(save_path, errflags, char)
    buf = open(save_path, "rb").read()[off:off + REG_LEN]
    if len(buf) != REG_LEN:
        sys.exit(f"error: {save_path} too short for char {char}")
    return buf


def set_flags(inv, buf, keep):
    """All flag ids SET in this region buffer (filtered)."""
    out = set()
    for rb in range(REG_LEN):
        if buf[rb] == 0:
            continue
        for k in range(8):
            if buf[rb] & (1 << (7 - k)):
                fid = decode(inv, rb, k)
                if fid and keep(fid):
                    out.add(fid)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("alive")
    ap.add_argument("dead")
    ap.add_argument("--bst", default="/tmp/ER-Save-Lib/src/res/eventflag_bst.txt")
    ap.add_argument("--errflags", default=DEFAULT_ERRFLAGS,
                    help="er-save-lib errflags binary (for per-file region offset)")
    ap.add_argument("--range", action="append", default=[])
    ap.add_argument("--char", type=int, default=0)
    ap.add_argument("--monotonic", action="append", default=[],
                    help="extra DEAD-state save(s); a flag is only reported if it is "
                         "ALSO set in every one of these. Filters region/proximity "
                         "flags that toggle false again when you leave the area — "
                         "use 1-2 saves taken at different spots after the kill.")
    a = ap.parse_args()

    inv = load_bst(a.bst)
    ranges = []
    for r in a.range:
        lo, hi = r.split("-")
        ranges.append((int(lo), int(hi)))

    def keep(fid):
        return not ranges or any(lo <= fid <= hi for lo, hi in ranges)

    alive = set_flags(inv, read_region(a.alive, a.errflags, a.char), keep)
    dead = set_flags(inv, read_region(a.dead, a.errflags, a.char), keep)
    newly = dead - alive
    cleared = alive - dead

    # Monotonic filter: keep only flags also set in EVERY extra dead-state save.
    for extra in a.monotonic:
        s = set_flags(inv, read_region(extra, a.errflags, a.char), keep)
        newly &= s

    label = " (monotonic-filtered)" if a.monotonic else ""
    print(f"newly SET (alive->dead){label}: {len(newly)}")
    for f in sorted(newly):
        print(f"  {f}")
    if not a.monotonic:
        print(f"newly CLEARED: {len(cleared)}")
        for f in sorted(cleared):
            print(f"  {f}")
        print("\ntip: a real death flag is MONOTONIC. Re-run with --monotonic "
              "<another-dead-save> (taken elsewhere) to drop region/proximity "
              "flags that toggle off when you leave; cross-check with errflags get.")


if __name__ == "__main__":
    main()
