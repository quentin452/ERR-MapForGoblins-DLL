#!/usr/bin/env python3
"""Pin NpcParam row size + byte offsets of itemLotId_map / itemLotId_enemy / nameId
for the live NpcParam read in the disk-loot ENEMY pass (npcId -> lot).

Loads regulation.bin NpcParam (with paramdef applied), then:
  (a) tries to read each field's byte offset from the Andre.SoulsFormats column API
      (param.Columns[*].ByteOffset or similar), and
  (b) EMPIRICALLY confirms by reading a known npcParamId's raw row bytes and locating
      the SoulsFormats cell value of itemLotId_* inside them.

Run: PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" \
     MFG_PROFILE=err py -3.14 tools/probe_npc_lot_offset.py
"""
import sys, io, os, struct, json, tempfile
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
PARAMDEF_DIR = config.PARAMDEF_DIR
_str_type = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)

def _read_param_bytes(data):
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_npc.param')
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = _param_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r

def load_paramdefs():
    defs = {}
    for xml_path in PARAMDEF_DIR.glob('*.xml'):
        try:
            pdef = SoulsFormats.PARAMDEF.XmlDeserialize(str(xml_path), False)
            if pdef and pdef.ParamType:
                defs[str(pdef.ParamType)] = pdef
        except Exception:
            pass
    return defs

def read_regulation():
    rb = config.require_err_mod_dir() / 'regulation.bin'
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(rb))
    return bnd

def find_npc_param(bnd, paramdefs):
    for f in bnd.Files:
        if 'NpcParam' in str(f.Name) and 'NpcThink' not in str(f.Name):
            param = _read_param_bytes(f.Bytes)
            pt = str(param.ParamType) if param.ParamType else ''
            if pt in paramdefs:
                param.ApplyParamdef(paramdefs[pt])
            return param
    return None

def dump_api(param):
    print("\n--- PARAM attributes probing ---")
    for attr in ('Columns', 'AppliedParamdef', 'DetectedSize', 'ParamType'):
        print(f"  has {attr}: {hasattr(param, attr)}")
    # column byte offsets
    cols = getattr(param, 'Columns', None)
    if cols:
        print(f"  Columns count: {len(list(cols))}")
        sample = list(cols)[0]
        print("  column 0 members:", [m for m in dir(sample) if not m.startswith('_')][:30])

# primitive byte size by DefType name
_PRIM = {'s8':1,'u8':1,'s16':2,'u16':2,'s32':4,'u32':4,'f32':4,'f64':8,'angle32':4,
         'b32':4,'dummy8':1,'fixstr':1,'fixstrW':2}
_BITTYPES = {'s8','u8','s16','u16','s32','u32','b32','dummy8'}

def _prim(dt):  # DefType -> primitive byte size
    return _PRIM.get(dt, 4)

def compute_offsets(param):
    """Replicate SoulsFormats PARAM cell-offset packing from AppliedParamdef.Fields."""
    pdef = param.AppliedParamdef
    offset = 0
    bit_offset = -1   # bits consumed in the current storage unit
    bit_prim = 0      # primitive byte-size of the current bit unit
    out = {}
    for fld in pdef.Fields:
        dt = str(fld.DisplayType)
        name = str(fld.InternalName)
        arr = int(getattr(fld, 'ArrayLength', 1) or 1)
        bitsize = int(getattr(fld, 'BitSize', -1))
        # A field is a bit-field iff it declares a BitSize. dummy8 with BitSize is
        # bit-PADDING that shares the unit. SF groups by the unit's bit LIMIT
        # (primitive size), NOT the DefType name — so u8(1) + dummy8(7) pack into 1 byte.
        is_bit = bitsize != -1 and dt in _BITTYPES
        if is_bit:
            prim = _prim(dt)
            limit = prim * 8
            if bit_offset == -1 or bit_prim != prim or bit_offset + bitsize > limit:
                bit_offset = 0
                bit_prim = prim
                out[name] = offset
                offset += prim
            else:
                out[name] = offset - bit_prim
            bit_offset += bitsize
        else:
            bit_offset = -1
            bit_prim = 0
            out[name] = offset
            sz = _prim(dt)
            if dt in ('dummy8', 'fixstr', 'fixstrW'):
                sz = _prim(dt) * arr
            offset += sz
    return out, offset

def main():
    paramdefs = load_paramdefs()
    bnd = read_regulation()
    param = find_npc_param(bnd, paramdefs)
    if param is None:
        print("NpcParam not found"); return
    print("ParamType:", str(param.ParamType))
    print("DetectedSize:", getattr(param, 'DetectedSize', '?'))
    dump_api(param)

    want = {'itemLotId_map', 'itemLotId_enemy', 'nameId', 'getSoul'}
    offs, total = compute_offsets(param)
    print(f"\n--- computed row size: {total} (DetectedSize {getattr(param,'DetectedSize','?')}) "
          f"{'MATCH' if total == int(getattr(param,'DetectedSize',-1)) else 'MISMATCH!'} ---")
    print("--- field byte offsets (computed from paramdef) ---")
    for k in want:
        v = offs.get(k)
        print(f"  {k}: {v if v is None else hex(v)}")
    # full field dump up to past itemLotId_map to locate any packing error
    print("\n--- first 60 fields (name | type | bits | arr | offset) ---")
    n = 0
    for fld in param.AppliedParamdef.Fields:
        nm = str(fld.InternalName)
        dt = str(fld.DisplayType)
        bs = int(getattr(fld, 'BitSize', -1))
        ar = int(getattr(fld, 'ArrayLength', 1) or 1)
        print(f"  {nm:22} {dt:8} bits={bs:3} arr={ar:3} @ {hex(offs.get(nm,0))}")
        n += 1
        if n >= 60: break

    # Empirical confirm: pick a few npc ids from the enemy table, read cell values
    enemy = json.load(open('.scratch/enemy_rows.json')) if os.path.exists('.scratch/enemy_rows.json') else []
    sample_npcs = []
    # also just take first rows with non-zero lots
    by_id = {}
    for row in param.Rows:
        by_id[int(row.ID)] = row
    shown = 0
    print("\n--- sample NpcParam rows (cell values) ---")
    for rid, row in list(by_id.items()):
        vmap = vene = None
        for cell in row.Cells:
            nm = str(cell.Def.InternalName)
            if nm == 'itemLotId_map': vmap = int(str(cell.Value))
            elif nm == 'itemLotId_enemy': vene = int(str(cell.Value))
        if (vmap and vmap > 0) or (vene and vene > 0):
            print(f"  npc {rid}: itemLotId_map={vmap} itemLotId_enemy={vene}")
            shown += 1
            if shown >= 10: break

if __name__ == '__main__':
    main()
