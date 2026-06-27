#!/usr/bin/env python3
"""Decompress a 01_common.sblytbnd.dcx and emit the FULL item-icon layout
iconId -> (sheet, x, y, w, h) from every <SubTexture name="MENU_ItemIcon_<id>.png">.
This is the complete, deterministic source for the no-bake item-icon rect table
(the live-RAM parse only sees currently-resident atlases). Usage:
   py -3.14 dump_item_icon_layout.py <path-to-01_common.sblytbnd.dcx> [out.json]"""
import sys, json, xml.etree.ElementTree as ET, re
from pathlib import Path
from collections import Counter
TOOLS = Path(__file__).parent
DLL = TOOLS / "lib" / "Andre.SoulsFormats.dll"
from pythonnet import load; load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.Reflection import Assembly, BindingFlags
asm = Assembly.LoadFrom(str(DLL)); clr.AddReference(str(DLL))
_str = SysType.GetType("System.String")
def _read(tn):
    return asm.GetType(tn).GetMethod("Read",
        BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str]), None)
path = sys.argv[1]
out  = sys.argv[2] if len(sys.argv) > 2 else None
bnd = _read("SoulsFormats.BND4").Invoke(None, Array[Object]([path]))
layout={}; per_sheet=Counter()
for f in bnd.Files:
    try:
        root = ET.fromstring(bytes(f.Bytes.ToArray()).decode("utf-8","replace"))
    except Exception: continue
    sheet = root.get("imagePath") or root.get("name") or ""
    for s in root.findall(".//SubTexture"):
        m = re.match(r"MENU_ItemIcon_(\d+)", s.get("name",""))
        if not m: continue
        iid=int(m.group(1))
        layout[iid]=(sheet, int(s.get("x",0)), int(s.get("y",0)),
                            int(s.get("width",0)), int(s.get("height",0)))
        per_sheet[sheet]+=1
print(f"# {path}")
print(f"# MENU_ItemIcon_* entries: {len(layout)}  (iconId range {min(layout)}..{max(layout)})")
print("# per sheet:")
for sh,c in sorted(per_sheet.items()): print(f"   {sh:24s} {c}")
ks=sorted(layout)
print("# sample:")
for kk in ks[:4]+ks[-4:]: print(f"   {kk} -> {layout[kk]}")
if out:
    with open(out,"w") as fh: json.dump({str(a):b for a,b in layout.items()},fh)
    print(f"# wrote {out}")
