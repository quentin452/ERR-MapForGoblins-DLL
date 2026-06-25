#!/usr/bin/env python3
"""RE the MSBE POINT-section (Region) entry layout for the no-bake Spirit Springs pass.

Replicates the C++ parser's 6-section walk (POINT = section 2), then for each region entry
pins: name@+0x00 (offset, eio), the Vector3 position offset (scan for SoulsFormats' known
pos), and the TYPE field that discriminates MountJump regions from other subtypes (compare the
header ints of MountJump vs non-MountJump entries). Pure disk datamining — no game."""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load
load('coreclr')
import clr
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
from pathlib import Path

_str = SysType.GetType('System.String')
_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


def rd32(b, o): return struct.unpack_from('<i', b, o)[0]
def rd64(b, o): return struct.unpack_from('<q', b, o)[0]
def rdf(b, o):  return struct.unpack_from('<f', b, o)[0]


def utf16(b, o):
    s = b''
    while o + 1 < len(b) and b[o:o+2] != b'\x00\x00':
        s += b[o:o+2]; o += 2
    return s.decode('utf-16-le', 'replace')


def sections(buf):
    """Return the 6 PARAM sections as (entries, entryArrOffset). Mirrors parse_msb."""
    assert buf[:4] == b'MSB ', "not an MSB"
    secs = []
    po = rd32(buf, 0x08)
    for _ in range(6):
        offsetCount = rd32(buf, po + 4)
        entries = offsetCount - 1
        entryArr = po + 0x10
        secs.append((entries, entryArr))
        po = rd64(buf, entryArr + entries * 8)
    return secs


# ── RESULT (pinned 2026-06-25, validated vs SoulsFormats over 651 _00 tiles) ──────────────
# MSBE POINT-section (Region) entry layout:
#   name     @ +0x00  (i64 offset, entry-relative on disk)
#   subtype  @ +0x08  (i32) — MountJumps=46  LockedMountJumps=54  Others=-1 (name-filtered)
#   position @ +0x14  (Vector3 float) — 100% match for MountJumps 48/48, Locked 5/5,
#                                       Others/FakeSpiritSpringJump 14/14
from collections import Counter, defaultdict
M = Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\mod\map\MapStudio")
pos_match = pos_total = 0          # position@+0x14 validation (all regions)
type08 = defaultdict(Counter)      # subtype-collection -> Counter(+0x08 value)
TARGET = ('MountJumps', 'LockedMountJumps', 'Others')
tgt_match = defaultdict(lambda: [0, 0])   # subtype -> [pos@0x14 match, total] (our targets)
files = 0
for p in sorted(M.glob('*.msb.dcx')):
    pr = p.name.replace('.msb.dcx', '').split('_')
    if len(pr) < 4 or pr[3] != '00':
        continue
    raw = bytes(SoulsFormats.DCX.Decompress(str(p)).ToArray())
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_rl.msb')
    SysFile.WriteAllBytes(tmp, raw); msb = _read.Invoke(None, Array[Object]([tmp])); os.unlink(tmp)
    files += 1
    # name -> (subtype collection name, pos) from SoulsFormats
    nmap = {}
    for cn in dir(msb.Regions):
        if cn.startswith('_'):
            continue
        try:
            coll = getattr(msb.Regions, cn)
            it = list(coll)
        except Exception:
            continue
        for r in it:
            try:
                nmap[str(r.Name)] = (cn, (float(r.Position.X), float(r.Position.Y), float(r.Position.Z)))
            except Exception:
                pass
    pt_entries, pt_arr = sections(raw)[2]
    for i in range(pt_entries):
        e = rd64(raw, pt_arr + i * 8)
        nm = utf16(raw, e + rd64(raw, e + 0x00))
        if nm not in nmap:
            continue
        cn, (px, py, pz) = nmap[nm]
        pos_total += 1
        ok = False
        try:
            ok = (abs(rdf(raw, e+0x14)-px) < 0.05 and abs(rdf(raw, e+0x18)-py) < 0.05
                  and abs(rdf(raw, e+0x1c)-pz) < 0.05)
        except Exception:
            pass
        if ok:
            pos_match += 1
        if cn in TARGET and (cn != 'Others' or 'FakeSpiritSpringJump' in nm):
            key = cn if cn != 'Others' else 'Others/FakeSpiritSpringJump'
            tgt_match[key][1] += 1
            tgt_match[key][0] += ok
        type08[cn][rd32(raw, e+0x08)] += 1

print(f"\nscanned {files} _00 tiles")
print(f"position @ +0x14 matches SoulsFormats for {pos_match}/{pos_total} regions (all subtypes)")
print("position @ +0x14 for OUR target subtypes (must be 100%):")
for k, (m, t) in tgt_match.items():
    print(f"  {k:30} {m}/{t}")
print("\n+0x08 value distribution per region subtype (the +0x08 int = subtype discriminator):")
for cn in sorted(type08, key=lambda c: -sum(type08[c].values())):
    vals = type08[cn]
    if cn in ('MountJumps', 'LockedMountJumps', 'Others') or sum(vals.values()) > 200:
        print(f"  {cn:24} {dict(vals.most_common(4))}")
