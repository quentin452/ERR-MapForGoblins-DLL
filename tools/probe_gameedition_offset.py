#!/usr/bin/env python3
"""Pin the GameEditionDisable byte offset in an MSBE Part entry, empirically vs SoulsFormats.

Result (2026-06-25): GameEditionDisable is an int32 at part entry +0x44 — the SOLE offset
matching across 6612 parts incl. 30 GED=1 positives. Used by src/worldmap/msbe_parser.cpp to
drop disabled (engine-never-spawns) placements, matching the bake's generators.

Method (per memory re-offset-validation: positive+negative samples, anchored on a known offset):
for every part, SoulsFormats gives (Position, GameEditionDisable); we locate the part entry in
the RAW decompressed bytes by its unique pos.x/y/z float window (= entry+0x20, the offset the
C++ parser already uses), then test each candidate int offset == GED across all parts."""
import sys, io, os, tempfile, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
from pathlib import Path
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

_str = SysType.GetType('System.String')
_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)


def rd32(b, o):
    return struct.unpack_from('<i', b, o)[0]


def find_pos_window(b, x, y, z):
    """Byte offsets where the 3 consecutive floats (x,y,z) appear = part entry +0x20."""
    pat = struct.pack('<fff', x, y, z)
    hits, start = [], 0
    while True:
        i = b.find(pat, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    return hits


def main():
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    # A handful of tiles incl. a known GED=1 one (m60_45_39_00) for positive samples.
    tiles = ['m60_45_39_00', 'm60_42_36_00', 'm60_38_52_00', 'm30_17_00_00']
    cand = {off: True for off in range(0x08, 0x80, 4)}
    samples = pos = 0
    for t in tiles:
        p = msb_dir / (t + '.msb.dcx')
        if not p.exists():
            continue
        raw = bytes(SoulsFormats.DCX.Decompress(str(p)).ToArray())
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_ged.msb')
        SysFile.WriteAllBytes(tmp, raw)
        msb = _read.Invoke(None, Array[Object]([tmp]))
        os.unlink(tmp)
        for cat_name in ('MapPieces', 'Assets', 'Enemies', 'DummyAssets', 'Collisions'):
            for part in getattr(msb.Parts, cat_name, None) or []:
                try:
                    ged = int(getattr(part, 'GameEditionDisable', 0) or 0)
                    x, y, z = float(part.Position.X), float(part.Position.Y), float(part.Position.Z)
                except Exception:
                    continue
                hits = find_pos_window(raw, x, y, z)
                if len(hits) != 1:   # ambiguous position → skip
                    continue
                base = hits[0] - 0x20
                if base < 0:
                    continue
                samples += 1
                pos += (ged == 1)
                for off in list(cand):
                    if base + off + 4 > len(raw) or rd32(raw, base + off) != ged:
                        cand[off] = False

    good = [hex(o) for o, ok in cand.items() if ok]
    print(f"samples={samples} (GED=1 positives={pos})")
    print(f"offsets matching GameEditionDisable 100%: {good}")


if __name__ == '__main__':
    main()
