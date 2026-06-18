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

REG_LEN = 0x1BF99F
HEADER = 4 + 0x2FC          # magic + PC header -> slot 0 start (errflags HEADER)
SLOT_SIZE = 0x280010
BLOCK = 125


def load_bst(path):
    inv = {}
    with open(path) as f:
        for line in f:
            b, r = line.split(",")
            inv[int(r)] = int(b)            # region_index -> block
    return inv


def region_off(char):
    # Matches er-save-lib: slot `char` event-flags start. The +0x... within a
    # slot is constant; errflags reported char0 @ 0x3689D, so derive the in-slot
    # delta once and apply per slot.
    in_slot = 0x3689D - HEADER             # event-flags delta inside slot 0
    return HEADER + char * SLOT_SIZE + in_slot


def decode(inv, rb, k):
    region_index, bb = divmod(rb, BLOCK)
    blk = inv.get(region_index)
    if blk is None:
        return None
    idx = bb * 8 + k
    if idx >= 1000:
        return None
    return blk * 1000 + idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("alive")
    ap.add_argument("dead")
    ap.add_argument("--bst", default="/tmp/ER-Save-Lib/src/res/eventflag_bst.txt")
    ap.add_argument("--range", action="append", default=[])
    ap.add_argument("--char", type=int, default=0)
    a = ap.parse_args()

    inv = load_bst(a.bst)
    off = region_off(a.char)
    A = open(a.alive, "rb").read()[off:off + REG_LEN]
    B = open(a.dead, "rb").read()[off:off + REG_LEN]
    if len(A) != REG_LEN or len(B) != REG_LEN:
        sys.exit("error: save too short / wrong char slot / wrong layout")

    ranges = []
    for r in a.range:
        lo, hi = r.split("-")
        ranges.append((int(lo), int(hi)))

    def keep(fid):
        return not ranges or any(lo <= fid <= hi for lo, hi in ranges)

    newly, cleared = [], []
    for rb in range(REG_LEN):
        x, y = A[rb], B[rb]
        if x == y:
            continue
        for k in range(8):
            m = 1 << (7 - k)
            fid = decode(inv, rb, k)
            if not fid or not keep(fid):
                continue
            if (y & m) and not (x & m):
                newly.append(fid)
            elif (x & m) and not (y & m):
                cleared.append(fid)

    print(f"newly SET (alive->dead): {len(newly)}")
    for f in sorted(newly):
        print(f"  {f}")
    print(f"newly CLEARED: {len(cleared)}")
    for f in sorted(cleared):
        print(f"  {f}")
    if not ranges:
        print("\ntip: re-run with --range 1042360000-1042369999 (the NPC's flag "
              "block) to cut the noise; cross-check the pick with errflags get.")


if __name__ == "__main__":
    main()
