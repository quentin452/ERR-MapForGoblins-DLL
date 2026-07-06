"""Probe 1b — per-map deep dump on known-water maps (far_water_surface_disk_re_findings.md §2-§3):
Collision part properties (HitFilterID etc.), MapPiece Y (all at origin?), Model taxonomy, HitFilterID
histogram water-vs-dry. Is there any non-nominal water discriminator in the MSB? (Answer: no.)
Usage: py _probe_water_deep.py [m14_00_00_00 ...]
"""
import os, sys, io, tempfile, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pythonnet import load; load("coreclr")
import clr, config
from System import Array, Object, Type as SysType
from System.IO import File as SysFile
from System.Reflection import Assembly, BindingFlags
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL)); clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
_str = SysType.GetType("System.String")
_read = asm.GetType("SoulsFormats.MSBE").GetMethod("Read",
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
GM = config.GAME_DIR / "map" / "mapstudio"

def load_msb(n):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + "_pd.msb")
    SysFile.WriteAllBytes(t, SoulsFormats.DCX.Decompress(str(GM / (n + ".msb.dcx"))).ToArray())
    m = _read.Invoke(None, Array[Object]([t])); os.unlink(t); return m

def props(o):
    d = {}
    for p in o.GetType().GetProperties():
        try:
            v = p.GetValue(o)
            if v is None: continue
            tn = type(v).__name__
            if tn in ("String", "Int32", "Byte", "Boolean", "Single", "Int16", "UInt32", "SByte", "Int64"):
                d[p.Name] = v
            elif tn == "Vector3":
                d[p.Name] = (round(float(v.X), 1), round(float(v.Y), 1), round(float(v.Z), 1))
        except Exception:
            pass
    return d

def main():
    maps = sys.argv[1:] or ["m14_00_00_00", "m12_01_00_00", "m10_00_00_00"]
    for mapn in maps:
        try:
            msb = load_msb(mapn)
        except Exception as e:
            print(mapn, "ERR", e); continue
        parts = list(msb.Parts.GetEntries()); models = list(msb.Models.GetEntries())
        print(f"\n########## {mapn}  parts={len(parts)} models={len(models)} ##########")
        mtax = collections.Counter(type(m).__name__ for m in models)
        print("MODEL subtypes:", dict(mtax))
        # MapPiece Position check (all at origin => Y is in the FLVER, not the MSB)
        mps = [p for p in parts if type(p).__name__ == "MapPiece"]
        nonzero = [p for p in mps if abs(p.Position.Y) > 0.5 or abs(p.Position.X) > 0.5 or abs(p.Position.Z) > 0.5]
        print(f"MapPiece: {len(mps)} parts, {len(nonzero)} with non-origin Position (rest are (0,0,0))")
        # Collision HitFilterID histogram + DisableTorrent
        cs = [p for p in parts if type(p).__name__ == "Collision"]
        hist = collections.Counter(); tor = collections.Counter()
        for p in cs:
            hist[p.HitFilterID] += 1
            if bool(p.DisableTorrent): tor[p.HitFilterID] += 1
        print(f"Collision: {len(cs)} parts  HitFilterID={dict(sorted(hist.items()))}  DisableTorrent-by-HFID={dict(sorted(tor.items()))}")
        if cs:
            print("  sample Collision props:", props(cs[0]))
        rtax = collections.Counter(type(r).__name__ for r in msb.Regions.GetEntries())
        print("Region subtypes:", dict(sorted(rtax.items(), key=lambda x: -x[1])))

if __name__ == "__main__":
    main()
