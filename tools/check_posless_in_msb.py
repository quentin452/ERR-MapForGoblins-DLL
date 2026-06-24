#!/usr/bin/env python3
"""Are the 'absent-from-EMEVD' position-less lots actually MSB Treasures (now
emitted by the #1 disk parser)? Scan every ERR _00 MSB's Treasure events and
intersect their itemLotIds with the 430 non-custom position-less lots."""
import sys, io, os, json, struct, zlib
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config

def dcx_to_msb(data):
    if data[:4] != b'DCX\0' or data[0x18:0x1c] != b'DCS\0': return None
    dca = data.find(b'DCA\0', 0x28)
    if dca < 0: return None
    z = data[dca + 8:]
    if data[0x28:0x2c] == b'DFLT':
        try: return zlib.decompress(z)
        except Exception: return None
    return None

def rd32(b, o): return struct.unpack_from('<I', b, o)[0]
def rd64(b, o): return struct.unpack_from('<Q', b, o)[0]

def treasure_lots(buf):
    po = rd32(buf, 0x08); secs = []
    for _ in range(6):
        n = rd32(buf, po + 4) - 1
        secs.append((n, po + 0x10)); po = rd64(buf, po + 0x10 + n * 8)
    ev_n, ev_a = secs[1]; out = set()
    for i in range(ev_n):
        e = rd64(buf, ev_a + i * 8)
        if rd32(buf, e + 0x0c) != 4: continue
        td = rd64(buf, e + 0x20)
        if not td: continue
        td = e + td
        lot = rd32(buf, td + 0x10)
        if lot not in (0, 0xffffffff): out.add(lot)
    return out

def main():
    db = json.load(open(config.PROJECT_DIR / 'data' / 'items_database.json'))
    posless = set(r['itemLotId'] for r in db
                  if r['x'] == 0 and r['y'] == 0 and r['z'] == 0
                  and r['map'] != 'm60_44_60_00')
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    all_treasure = set()
    for p in sorted(msb_dir.glob('*_00.msb.dcx')):
        msb = dcx_to_msb(p.read_bytes())
        if msb and msb[:4] == b'MSB ':
            all_treasure |= treasure_lots(msb)
    inter = posless & all_treasure
    print(f"non-custom position-less lots: {len(posless)}")
    print(f"distinct MSB Treasure lots (all _00 DFLT maps): {len(all_treasure)}")
    print(f"position-less lots that ARE MSB Treasures (now emitted by #1): {len(inter)}")
    if inter:
        print("  sample:", sorted(inter)[:30])
    only = posless - all_treasure
    print(f"position-less lots NOT in any MSB Treasure: {len(only)}")

if __name__ == '__main__':
    main()
