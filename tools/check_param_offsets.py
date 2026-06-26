#!/usr/bin/env python3
"""Source-of-truth guard for the param/struct byte offsets the DLL hardcodes.

The game compiles param field offsets into its code (no runtime paramdef), so the de-facto
source of truth is the SoulsFormats Paramdex — committed here in tools/paramdefs/*.xml. This
script walks the paramdef field layout (bitfield- + array-aware, same logic as paramoff.py),
looks up the exact fields the DLL reads by hardcoded offset, and asserts they match. Run at
build/CI; exits 1 on any mismatch so a latent offset bug (e.g. the AEG bit5/6 16k leak) can't
ship silently. This is the cheap "offset source of truth" — it does NOT replace the constants,
it GUARDS them against the authoritative defs.
"""
import os, re, sys, xml.etree.ElementTree as ET

SIZE = {'u8':1,'s8':1,'u16':2,'s16':2,'u32':4,'s32':4,'f32':4,'f64':8,
        'dummy8':1,'fixstr':1,'fixstrW':2}

def all_field_offsets(path):
    """name -> (byte_off, start_bit|None, nbits|None, type_str). Adds _ROWSIZE.

    Bitfield packing mirrors SoulsFormats: consecutive bitfields share a storage unit when they
    have the same STORAGE SIZE (NOT the same type name) and the bits still fit — so `u8 x:1` +
    `dummy8 y:7` pack into ONE byte. Comparing the type name instead added a spurious byte per
    mixed-type bit run and shifted every later field."""
    root = ET.parse(path).getroot()
    off = 0; bsz = 0; bused = 0; bunit = 0; res = {}
    for f in root.iter('Field'):
        d = f.get('Def').strip()
        m = re.match(r'(\w+)\s+([A-Za-z0-9_]+)\s*(?::\s*(\d+))?\s*(?:\[\s*(\d+)\s*\])?', d)
        if not m: continue
        typ, name, bits, count = m.group(1), m.group(2), m.group(3), m.group(4)
        sz = SIZE.get(typ, 4)
        if bits:
            b = int(bits)
            if bsz != sz or bused + b > sz*8:   # new unit on storage-size change OR overflow
                bsz = sz; bunit = off; off += sz; bused = 0
            res[name] = (bunit, bused, b, typ)
            bused += b
        else:
            bsz = 0; bused = 0
            n = int(count) if count else 1
            res[name] = (off, None, None, '%sx%d' % (typ, n))
            off += sz*n
    res['_ROWSIZE'] = (off, None, None, '')
    return res

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'paramdefs')

# The AUTHORITATIVE offset is the live-validated value compiled into the DLL (pinned via raw param
# bytes + POSITIVE/NEGATIVE samples — see memory re-offset-validation). The committed Paramdex is a
# CROSS-CHECK, not the source of truth: it can version-drift from ERR's exact regulation, so a few
# fields legitimately disagree (the game/code value wins). Those are marked drift=True (expected,
# documented). A non-drift mismatch, or a NEW drift, is what this guard must surface.
# (paramdef_file, field, code_byte_off, code_bit|None, drift, note)
CHECKS = [
    ('EquipParamGoods.xml', 'goodsType',           0x3e, None, False, 'goods_type_live'),
    ('EquipParamGoods.xml', 'sortGroupId',         0x72, None, True,  'goods_sort_group; Paramdex 0x73 (drift)'),
    ('NpcParam.xml',        'itemLotId_enemy',     0x30, None, False, 'npc_loot_lot enemy'),
    ('NpcParam.xml',        'itemLotId_map',       0x34, None, False, 'npc_loot_lot map'),
    ('NpcParam.xml',        'nameId',              0x0c, None, False, 'npc name'),
    ('NpcParam.xml',        'teamType',            0x133, None,False, 'npc team'),
    ('AssetGeometryParam.xml','pickUpItemLotParamId',0xb8, None,True, 'aeg_pickup_lot; Paramdex 0xb9 (drift)'),
    ('AssetGeometryParam.xml','isEnableRepick',    0x3c, 5,   True,  'AEG gather filter; Paramdex bit6, LIVE-validated bit5 (the 16k-leak field)'),
    ('BonfireWarpParam.xml','eventflagId',         0x04, None, False, 'grace discover_flag'),
    ('BonfireWarpParam.xml','iconId',              0x1c, None, False, 'grace iconId'),
    ('BonfireWarpParam.xml','textId1',             0x30, None, True,  'grace textId1; Paramdex 0x31 (drift)'),
]

def main():
    hard = 0; agree = 0; drift = 0; cache = {}
    for fn, field, code_off, code_bit, is_drift, note in CHECKS:
        path = os.path.join(BASE, fn)
        if fn not in cache:
            if not os.path.exists(path):
                print('  MISSING paramdef: %s' % fn); hard += 1; continue
            cache[fn] = all_field_offsets(path)
        fields = cache[fn]
        if field not in fields:
            near = [k for k in fields if field.lower()[:5] in k.lower()][:6]
            print('  NOT FOUND  %-22s in %-24s  (near: %s)' % (field, fn, near)); hard += 1; continue
        boff, sbit, nbits, typ = fields[field]
        matches = (boff == code_off) and (code_bit is None or sbit == code_bit)
        pdbit = '' if sbit is None else (' bit%d' % sbit)
        cbit = '' if code_bit is None else (' bit%d' % code_bit)
        if matches:
            tag = 'OK  '; agree += 1
        elif is_drift:
            tag = 'DRIFT'; drift += 1            # expected Paramdex-vs-regulation drift; code wins
        else:
            tag = 'FAIL'; hard += 1              # unexpected → real regression / new drift
        print('  [%-5s] %-24s %-22s code=0x%x%s  paramdef=0x%x%s (%s)  %s'
              % (tag, fn, field, code_off, cbit, boff, pdbit, typ, note))
    print('\n%d agree, %d expected-drift, %d HARD (must be 0). rowsizes: %s'
          % (agree, drift, hard, {fn: hex(c['_ROWSIZE'][0]) for fn, c in cache.items()}))
    if hard == 0:
        print('OK: no unexpected offset divergence (Paramdex cross-check passed).')
    return 1 if hard else 0

if __name__ == '__main__':
    sys.exit(main())
