#!/usr/bin/env python3
"""OFFLINE PROTOTYPE of the mod-agnostic quest-NPC extractor.

Mines the decompiled EMEVD corpus for the ENGINE-STANDARD common-event templates
that wire every quest NPC, instead of hand-decoding each NPC:

  InitializeCommonEvent(0, 90005702, ENTITY, CONCLUDED_FLAG, REG_LO, REG_HI)
      90005702 = the shared NPC "_q99 concluded / died" handler.
  InitializeCommonEvent(0, 90005860, ENTITY, ...)   # boss-death variant (flag == entity)

Group the calls by (concluded_flag, reg_lo, reg_hi): each group == one quest NPC,
its entity list == all that NPC's map placements. This is what a RUNTIME version
would do by parsing the ACTIVE install's .emevd.dcx (via the DLL's dvdbnd reader),
so it is automatically correct for any mod that adds/removes/relocates NPCs --
NOT an ERR-frozen bake.

Usage: py extract_quest_npcs.py            # prints the table + validates the 3 known NPCs
       py extract_quest_npcs.py <js_dir>   # default D:/tools/emevd_js/err
"""
import re, sys, io
from pathlib import Path
from collections import defaultdict
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

JS = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'D:\tools\emevd_js\err')

# InitializeCommonEvent(slot, template, entity, arg1, arg2, ...)  (also InitializeEvent)
CALL = re.compile(r'Initialize(?:Common)?Event\(\s*\d+\s*,\s*(9000570[0-9]|90005860)\s*,\s*([\d,\s\-]+?)\)')

def main():
    # entity -> per-template arg lists
    calls = defaultdict(lambda: defaultdict(list))   # entity -> {template -> args}
    npc = defaultdict(lambda: {'entities': set(), 'maps': set()})  # (concluded,lo,hi) -> info
    for p in sorted(JS.glob('*.emevd.dcx.js')):
        mp = p.name.replace('.emevd.dcx.js', '')
        for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
            for m in CALL.finditer(line):
                tpl = int(m.group(1))
                args = [int(a) for a in m.group(2).split(',') if a.strip().lstrip('-').isdigit()]
                if not args:
                    continue
                ent = args[0]
                calls[ent][tpl].append(args[1:])
                if tpl == 90005702 and len(args) >= 4:
                    concluded, lo, hi = args[1], args[2], args[3]
                    key = (concluded, lo, hi)
                    npc[key]['entities'].add(ent)
                    npc[key]['maps'].add(mp)

    print(f"scanned {len(list(JS.glob('*.emevd.dcx.js')))} EMEVD files")
    print(f"quest NPCs found (distinct concluded/register groups via 90005702): {len(npc)}\n")
    print(f"{'concluded':>10} {'register':>13} {'#ent':>5}  entities (first 6)")
    print('-' * 78)
    for (concluded, lo, hi), info in sorted(npc.items()):
        ents = sorted(info['entities'])
        print(f"{concluded:>10} {lo:>6}-{hi:<6} {len(ents):>5}  {ents[:6]}")

    # ---- validation vs the 3 hand-mapped NPCs ----
    print("\n=== validation vs manual mappings ===")
    expect = {'Boc': (3943, 3940, 3944), 'Thops': (3803, 3800, 3803), 'Alexander': (3663, 3660, 3663)}
    for name, key in expect.items():
        info = npc.get(key)
        ok = 'OK' if info else 'MISSING'
        ents = sorted(info['entities'])[:8] if info else []
        print(f"  {name:10} concluded={key[0]} reg={key[1]}-{key[2]}  -> {ok}  ({len(info['entities']) if info else 0} placements) {ents}")

if __name__ == '__main__':
    main()
