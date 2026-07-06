"""Probe 1 — how is WATER represented in ER MSB map files? (far_water_surface_disk_re_findings.md §1)
Dumps, per map: Part-subtype + Region-subtype taxonomy, and every Part/Model/Region whose name/model
matches a water token. Read-only, vanilla UXM game_dir (base geometry = mod-agnostic).
Usage: py _probe_water_msb.py [glob ...]   (default = legacy + underground + overworld sample)
"""
import os, sys, io, tempfile, glob as _glob, pathlib
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
_msbe_read = asm.GetType("SoulsFormats.MSBE").GetMethod(
    "Read", BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)

def rfb(data):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + "_probe.msb")
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, "ToArray") else data)
    r = _msbe_read.Invoke(None, Array[Object]([t])); os.unlink(t); return r

def prop(o, name):
    p = o.GetType().GetProperty(name)
    return p.GetValue(o) if p else None

def vec3(v):
    return None if v is None else (float(v.X), float(v.Y), float(v.Z))

# water tokens: english + FromSoft romaji (umi=sea, mizu=water, kawa/gawa=river, numa=swamp, taki=waterfall)
TOKENS = ("water", "umi", "mizu", "kawa", "gawa", "numa", "taki", "lake", "sea", "pond", "wet", "swamp", "moat", "river")
def hit(s):
    return bool(s) and any(t in s.lower() for t in TOKENS)

GAME_MAP = config.GAME_DIR / "map" / "mapstudio"

def default_globs():
    g = []
    for p in sorted(GAME_MAP.glob("m1?_00_00_00.msb.dcx")): g.append(p)
    for p in sorted(GAME_MAP.glob("m12_0?_00_00.msb.dcx")): g.append(p)  # underground rivers
    ow = sorted(GAME_MAP.glob("m60_*_02.msb.dcx"))
    g += ow[::20][:20]
    seen = set(); out = []
    for p in g:
        if p not in seen: seen.add(p); out.append(p)
    return out

def main():
    if len(sys.argv) > 1:
        paths = []
        for a in sys.argv[1:]:
            paths += [pathlib.Path(x) for x in _glob.glob(str(GAME_MAP / a))]
    else:
        paths = default_globs()
    print(f"# probing {len(paths)} maps for water representation\n")
    part_tax = {}; region_tax = {}; water_models_global = set()
    for mp in paths:
        try:
            msb = rfb(SoulsFormats.DCX.Decompress(str(mp)))
            parts = list(msb.Parts.GetEntries()); models = list(msb.Models.GetEntries())
            regions = list(msb.Regions.GetEntries())
        except Exception as e:
            print(f"  !! {mp.name}: {e}"); continue
        name = mp.name.replace(".msb.dcx", "")
        for p in parts: part_tax[type(p).__name__] = part_tax.get(type(p).__name__, 0) + 1
        for r in regions: region_tax[type(r).__name__] = region_tax.get(type(r).__name__, 0) + 1
        wparts = [(type(p).__name__, prop(p, "Name"), prop(p, "ModelName"), vec3(prop(p, "Position")))
                  for p in parts if hit(prop(p, "Name")) or hit(prop(p, "ModelName"))]
        wmodels = [m.Name for m in models if hit(getattr(m, "Name", None))]
        for wm in wmodels: water_models_global.add(wm)
        wregions = [(type(r).__name__, prop(r, "Name"), vec3(prop(r, "Position")))
                    for r in regions if hit(prop(r, "Name")) or hit(type(r).__name__)]
        if wparts or wmodels or wregions:
            print(f"== {name}  (parts={len(parts)} models={len(models)} regions={len(regions)}) ==")
            if wmodels: print(f"   water MODELS: {wmodels}")
            for t, nm, mdl, pos in wparts[:40]:
                print(f"   PART {t:16} name={nm:22} model={str(mdl):16} Y={pos[1] if pos else '?'}  pos={pos}")
            for t, nm, pos in wregions[:40]:
                print(f"   REGION {t:20} name={nm}  Y={pos[1] if pos else '?'}")
            print()
    print("\n===== PART SUBTYPE TAXONOMY =====")
    for k, v in sorted(part_tax.items(), key=lambda x: -x[1]): print(f"  {k:22} {v}")
    print("\n===== REGION SUBTYPE TAXONOMY =====")
    for k, v in sorted(region_tax.items(), key=lambda x: -x[1]): print(f"  {k:26} {v}")
    print(f"\n===== ALL WATER-TOKEN MODELS ({len(water_models_global)}) =====")
    for m in sorted(water_models_global): print(f"  {m}")

if __name__ == "__main__":
    main()
