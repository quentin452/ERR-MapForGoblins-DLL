#!/usr/bin/env python3
"""Decompress a .dcx OFFLINE and report where the layout XML hides in the RAW decompressed
bytes (what the runtime Oodle hook sees) vs what SoulsFormats finds per-entry. Answers why
hasLayout=false at runtime (e.g. the .layout entries are individually compressed)."""
import sys
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
_ba = SysType.GetType("System.Byte[]")

def _m(tn, name, argt):
    return asm.GetType(tn).GetMethod(name,
        BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy, None,
        Array[SysType](argt), None)

def find_all(hay: bytes, needle: bytes, cap=20):
    out, i = [], 0
    while True:
        j = hay.find(needle, i)
        if j < 0: break
        out.append(j); i = j + 1
        if len(out) >= cap: break
    return out

for path in sys.argv[1:]:
    print(f"\n======== {path} ========")
    res = _m("SoulsFormats.DCX","Decompress",[_str]).Invoke(None, Array[Object]([path]))
    raw = bytes(res.ToArray()) if hasattr(res, "ToArray") else bytes(res)
    print(f"decompressed {len(raw)} bytes, magic={raw[:4]!r}")
    for needle in (b"<SubTexture", b"MENU_MAP_", b".layout", b"DCX\x00", b"BND4"):
        hits = find_all(raw, needle)
        print(f"  raw contains {needle!r}: {len(hits)} hit(s) {hits[:8]}")
    # If it's a BND4, read it via SoulsFormats and inspect each entry's RAW bytes (is the
    # entry itself compressed? does its data contain <SubTexture in the clear?).
    if raw[:4] == b"BND4":
        bnd = _m("SoulsFormats.BND4","Read",[_str]).Invoke(None, Array[Object]([path]))
        files = list(bnd.Files)
        print(f"  BND4: {len(files)} files")
        shown = 0
        for f in files:
            data = bytes(f.Bytes.ToArray())
            name = str(f.Name)
            sub = data.find(b"<SubTexture")
            head = data[:8]
            if name.endswith(".layout") and shown < 6:
                shown += 1
                print(f"    {name}: {len(data)}B head={head!r} <SubTexture@={sub} "
                      f"(entry {'PLAINTEXT' if sub>=0 else 'COMPRESSED/other'})")
