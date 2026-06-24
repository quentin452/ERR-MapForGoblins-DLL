#!/usr/bin/env python3
"""Validate the "keep type-9 DummyAsset with EntityID!=0 or group!=0" rule
across ALL ERR _00 maps, by raw parse (mirrors msbe_parser + the new entity
read @ part+0x60 -> {EntityID@+0x00, EntityGroupIDs[8]@+0x1c}).

Checks the rule recovers reachable_dummy lots WITHOUT re-introducing the 305
genuinely-unreachable type-9 lots (the 178->21 win must hold):
  - reachable type-9 lots should NOT be in unreachable_msb_lots.json
  - inert type-9 lots should mostly BE in it
  - targets 4910 / 15000990 recovered; 2046460000 has no entity (cannot)."""
import sys, io, os, json, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config

GAME_MSB = config.require_err_mod_dir() / 'map' / 'MapStudio'
UNREACH = set(json.load(open(config.PROJECT_DIR / 'data' / 'unreachable_msb_lots.json')))

# DFLT (zlib) decompress in pure python; KRAK maps are skipped (loot is all DFLT).
import zlib
def dcx_to_msb(data):
    if data[:4] != b'DCX\0' or data[0x18:0x1c] != b'DCS\0':
        return None
    fmt = data[0x28:0x2c]
    dca = data.find(b'DCA\0', 0x28)
    if dca < 0: return None
    z = data[dca + 8:]
    if fmt == b'DFLT':
        try: return zlib.decompress(z)
        except Exception: return None
    return None  # KRAK -> skip (treasureless / no loot)

def rd32(b, o): return struct.unpack_from('<I', b, o)[0]
def rd64(b, o): return struct.unpack_from('<Q', b, o)[0]

def parse(buf):
    """yield (itemLotId, partType, entityId, group_nonzero) per Treasure w/ part."""
    po = rd32(buf, 0x08); secs = []
    for _ in range(6):
        entries = rd32(buf, po + 4) - 1
        entry_arr = po + 0x10
        secs.append((entries, entry_arr))
        po = rd64(buf, entry_arr + entries * 8)
    ev_n, ev_a = secs[1]; pt_n, pt_a = secs[5]
    for i in range(ev_n):
        e = rd64(buf, ev_a + i * 8)
        if rd32(buf, e + 0x0c) != 4: continue
        td_off = rd64(buf, e + 0x20)
        if not td_off: continue
        td = e + td_off
        lot = rd32(buf, td + 0x10)
        if lot in (0, 0xffffffff): continue
        pidx = rd32(buf, td + 0x08)
        if pidx == 0xffffffff or pidx >= pt_n:
            yield (lot, -1, 0, False); continue
        pe = rd64(buf, pt_a + pidx * 8)
        ptype = rd32(buf, pe + 0x0c)
        eid, gnz = 0, False
        ent_rel = rd64(buf, pe + 0x60)
        if 0 < ent_rel < 0x100000 and pe + ent_rel + 0x3c <= len(buf):
            ent = pe + ent_rel
            eid = rd32(buf, ent)
            for k in range(0x1c, 0x3c, 4):
                g = rd32(buf, ent + k)
                if g not in (0, 0xffffffff): gnz = True; break
        yield (lot, ptype, eid, gnz)

def main():
    reachable, inert = {}, {}   # lot -> example map
    for p in sorted(GAME_MSB.glob('*_00.msb.dcx')):
        data = p.read_bytes()
        msb = dcx_to_msb(data)
        if msb is None or msb[:4] != b'MSB ':
            continue
        for lot, ptype, eid, gnz in parse(msb):
            if ptype != 9:  # only DummyAsset placements
                continue
            if eid or gnz:
                reachable.setdefault(lot, p.name)
            else:
                inert.setdefault(lot, p.name)

    # A lot with BOTH a reachable and an inert dummy part -> reachable.
    for lot in list(inert):
        if lot in reachable:
            del inert[lot]

    reach_in_unreach = sorted(l for l in reachable if l in UNREACH)
    inert_in_unreach = [l for l in inert if l in UNREACH]
    print(f"type-9 DummyAsset lots: reachable(eid/grp!=0)={len(reachable)}  "
          f"inert(no entity)={len(inert)}")
    print(f"  reachable lots ALSO in unreachable_msb_lots.json (would be wrongly "
          f"KEPT): {len(reach_in_unreach)}")
    if reach_in_unreach:
        print(f"    {reach_in_unreach[:40]}")
    print(f"  inert lots in unreachable list (correctly dropped): "
          f"{len(inert_in_unreach)}/{len(inert)}")
    print()
    for t in (4910, 15000990, 2046460000):
        where = ('REACHABLE ' + reachable[t]) if t in reachable else \
                ('INERT ' + inert[t]) if t in inert else 'NOT a type-9 treasure'
        print(f"  target {t}: {where}  (in unreachable list: {t in UNREACH})")

if __name__ == '__main__':
    main()
