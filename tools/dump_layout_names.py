#!/usr/bin/env python3
"""Dump SubTexture names from a sblytbnd.dcx (no DDS decode) to inspect the
map-point icon naming scheme (does the name encode WORLD_MAP_POINT_PARAM.iconId?)."""
import os, sys, xml.etree.ElementTree as ET
from pathlib import Path
TOOLS = Path(__file__).parent
DLL = TOOLS / "lib" / "Andre.SoulsFormats.dll"
from pythonnet import load
load("coreclr")
import clr
from System import Array, Object
from System import Type as SysType
from System.Reflection import Assembly, BindingFlags
asm = Assembly.LoadFrom(str(DLL))
clr.AddReference(str(DLL))
_str = SysType.GetType("System.String")
def _read(tn):
    return asm.GetType(tn).GetMethod("Read",
        BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str]), None)
path = sys.argv[1]
bnd = _read("SoulsFormats.BND4").Invoke(None, Array[Object]([path]))
print(f"# {path}")
print(f"# {len(list(bnd.Files))} layout files")
for f in bnd.Files:
    name = str(f.Name)
    try:
        xml = bytes(f.Bytes.ToArray()).decode("utf-8", "replace")
        root = ET.fromstring(xml)
    except Exception as e:
        print(f"\n## {name}  (parse fail: {e})"); continue
    subs = root.findall(".//SubTexture")
    # the layout's referenced sheet (imageName attr on root or first sub), if present
    sheet = root.get("imagePath") or root.get("name") or ""
    print(f"\n## {os.path.basename(name)}  sheet='{sheet}'  {len(subs)} subtex")
    for s in subs:
        print(f"   {s.get('name')}  x={s.get('x')} y={s.get('y')} w={s.get('width')} h={s.get('height')}")
