#!/usr/bin/env python3
"""Pin the MSBE PART-entry EntityID/EntityGroupIDs offset for the disk-loot parser.

For the 3 "reachable_dummy" lots the DummyAsset filter currently drops
(4910 m12 / 15000990 m15 / 2046460000 m61), this:
  (a) reads the AUTHORITATIVE EntityID + EntityGroupIDs via SoulsFormats, then
  (b) replicates msbe_parser.parse_msb's raw section walk over the decompressed
      blob, locates the same PART entry, and brute-forces which entry-relative
      u64 sub-struct offset (or inline field) leads to that EntityID — pinning
      the byte offset the C++ parser needs to KEEP type-9 parts with Entity!=0.

Run: PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" MFG_PROFILE=err \
     py -3.14 tools/probe_entity_offset.py
"""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

TARGETS = {4910, 15000990, 2046460000}

# ── raw readers (mirror msbe_parser.cpp) ──────────────────────────────────
def rd32(b, o): return struct.unpack_from('<I', b, o)[0]
def rd64(b, o): return struct.unpack_from('<Q', b, o)[0]

EVENT_TYPE_TREASURE = 4
SEC_EVENT, SEC_PARTS = 1, 5

def walk_sections(buf):
    secs = []
    po = rd32(buf, 0x08)
    for _ in range(6):
        offset_count = rd32(buf, po + 4)
        entries = offset_count - 1
        entry_arr = po + 0x10
        secs.append((entries, entry_arr))
        po = rd64(buf, entry_arr + entries * 8)
    return secs

def raw_part_entries(buf):
    """index -> (entry_off, partName)."""
    secs = walk_sections(buf)
    pt_entries, pt_arr = secs[SEC_PARTS]
    out = {}
    for i in range(pt_entries):
        pe = rd64(buf, pt_arr + i * 8)
        nm_off = pe + rd64(buf, pe + 0x00)
        s = bytearray()
        o = nm_off
        while o + 1 < len(buf):
            c = buf[o] | (buf[o + 1] << 8)
            if c == 0: break
            s.append(c if c < 0x80 else ord('?'))
            o += 2
        out[i] = (pe, s.decode('ascii', 'replace'))
    return out

def raw_treasure_partidx(buf, lot_id):
    """Return list of partIndex for Treasure events whose itemLotId == lot_id."""
    secs = walk_sections(buf)
    ev_entries, ev_arr = secs[SEC_EVENT]
    hits = []
    for i in range(ev_entries):
        e = rd64(buf, ev_arr + i * 8)
        if rd32(buf, e + 0x0c) != EVENT_TYPE_TREASURE:
            continue
        td_off = rd64(buf, e + 0x20)
        if td_off == 0:
            continue
        td = e + td_off  # disk: entry-relative
        if rd32(buf, td + 0x10) == lot_id:
            hits.append(rd32(buf, td + 0x08))
    return hits

def decompress(path):
    return bytes(SoulsFormats.DCX.Decompress(str(path)).ToArray())

def read_msb(path):
    data = decompress(path)
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_pe.msb')
    SysFile.WriteAllBytes(tmp, data)
    msb = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return msb, data

def sf_part_by_name(msb, name):
    for cat in ('MapPieces', 'Enemies', 'Players', 'Collisions', 'DummyAssets',
                'DummyEnemies', 'ConnectCollisions', 'Assets'):
        col = getattr(msb.Parts, cat, None)
        if not col: continue
        for p in col:
            if str(p.Name) == name:
                return cat, p
    return None, None

def sf_treasures(msb):
    """list of (itemLotId, partName) for ItemLotID in TARGETS."""
    out = []
    for t in msb.Events.Treasures:
        try:
            lot = int(getattr(t, 'ItemLotID', getattr(t, 'ItemLotID1', 0)) or 0)
        except Exception:
            lot = 0
        if lot in TARGETS:
            pn = getattr(t, 'TreasurePartName', None)
            out.append((lot, str(pn) if pn else None))
    return out

def entity_groups(p):
    g = getattr(p, 'EntityGroupIDs', None)
    if g is None: return []
    try: return [int(x) for x in g]
    except Exception: return []

def probe_offset(buf, pe, target_eid):
    """Find where target_eid lives relative to part entry pe.
    Reports: inline u32 hits, and entry-relative u64 sub-struct pointers whose
    leading u32 == target_eid (the EntityID is the first field of its struct)."""
    findings = []
    # inline scan: target_eid as u32 anywhere in pe..pe+0x100
    for off in range(0, 0x100, 4):
        if pe + off + 4 > len(buf): break
        if rd32(buf, pe + off) == target_eid:
            findings.append(('inline u32', off))
    # sub-struct pointer scan: every u64 in pe+0x10..pe+0x90 as entry-rel offset
    for off in range(0x10, 0x90, 8):
        if pe + off + 8 > len(buf): break
        rel = rd64(buf, pe + off)
        if rel == 0 or rel > 0x100000: continue
        sub = pe + rel
        if sub + 4 > len(buf): continue
        if rd32(buf, sub) == target_eid:
            findings.append(('subptr@+0x%02x -> sub+0x00' % off, off))
    return findings

def hexwin(buf, base, n=0x40):
    chunk = buf[base:base + n]
    return ' '.join('%08x' % rd32(chunk, i) for i in range(0, len(chunk) - 3, 4))

def main():
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    print(f"MSB dir: {msb_dir}")
    areas = {'m12': [], 'm15': [], 'm61': []}
    for p in sorted(msb_dir.glob('*.msb.dcx')):
        a = p.name[:3]
        if a in areas and p.name.endswith('_00.msb.dcx'):
            areas[a].append(p)

    for area, files in areas.items():
        for path in files:
            try:
                msb, buf = read_msb(path)
            except Exception as ex:
                continue
            ts = sf_treasures(msb)
            if not ts:
                continue
            for lot, pname in ts:
                print('\n' + '=' * 78)
                print(f"LOT {lot}  in {path.name}  treasurePart={pname!r}")
                if not pname:
                    print("  (no TreasurePartName — item-glow/region)")
                    continue
                cat, part = sf_part_by_name(msb, pname)
                eid = int(getattr(part, 'EntityID', 0) or 0) if part else 0
                grp = entity_groups(part) if part else []
                print(f"  SoulsFormats: cat={cat}  EntityID={eid}  groups={grp}")
                # locate the raw part entry by name
                raw = raw_part_entries(buf)
                pe = None
                for i, (off, nm) in raw.items():
                    if nm == pname:
                        pe = off
                        print(f"  raw PART entry idx={i} @ file 0x{off:x}  "
                              f"partType@+0x0c={rd32(buf, off + 0x0c)}")
                        break
                if pe is None:
                    print("  (could not find raw entry by name)")
                    continue
                # probe EntityID offset
                if eid:
                    f = probe_offset(buf, pe, eid)
                    print(f"  EntityID {eid} located at: {f or 'NOT FOUND'}")
                # probe each non-zero group
                for gv in grp:
                    if gv and gv != -1:
                        f = probe_offset(buf, pe, gv)
                        print(f"  group {gv} located at: {f or 'NOT FOUND'}")
                # dump candidate sub-struct pointers
                print("  u64 fields pe+0x10..0x88 (entry-rel offset -> sub leading u32s):")
                for off in range(0x10, 0x90, 8):
                    rel = rd64(buf, pe + off)
                    tag = ''
                    if 0 < rel < 0x100000 and pe + rel + 16 <= len(buf):
                        sub = pe + rel
                        tag = ' -> sub[' + ' '.join('%d' % rd32(buf, sub + k)
                                                    for k in range(0, 16, 4)) + ']'
                    print(f"    +0x{off:02x} = 0x{rel:x}{tag}")

if __name__ == '__main__':
    main()
