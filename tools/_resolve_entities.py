#!/usr/bin/env python3
"""Ad-hoc: resolve MSB entity IDs -> NPC names (entity -> NPCParamID -> nameId
-> NpcName). Reuses the pipeline's SoulsFormats setup. Temp helper for the
EMEVD death-flag entity bridge (Irina/Edgar/Jerren/Gostoc). Reads VANILLA
game files (these are vanilla NPCs; complete MSB set)."""
import sys, io, os, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
import SoulsFormats

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str]), None)
def _get_read_str(type_name):
    return asm.GetType(type_name).GetMethod('Read',
        BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
        None, Array[SysType]([_str]), None)
_msbe_read = _get_read_str('SoulsFormats.MSBE')
_bnd4_read = _get_read_str('SoulsFormats.BND4')
_fmg_read  = _get_read_str('SoulsFormats.FMG')

def _read_from_bytes(read_method, data, suffix='.bin'):
    from System.IO import File as SysFile
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_re_fmg' + suffix)
    SysFile.WriteAllBytes(tmp, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = read_method.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return r

def load_npcname_fmg(game):
    """nameId -> name from menu.msgbnd's NpcName.fmg (+ DLC variants)."""
    import re
    names = {}
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = game / 'msg' / 'engus' / mb
        if not p.exists():
            continue
        bnd = _read_from_bytes(_bnd4_read, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in bnd.Files:
            base = str(f.Name).replace('\\', '/').rsplit('/', 1)[-1]
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = _read_from_bytes(_fmg_read, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]':
                        names[int(e.ID)] = t
    return names

# Targets: Irina/Edgar pair + Castle Morne instances + the two unresolved
# collisions (Jerren/Millicent share m15; Gostoc/Nepheli share 10000730),
# plus known anchors Seluvis(1034500701)/Iji(1034500710) as a sanity check.
TARGETS = {1045340700, 1045340705, 1043310705, 1044330705,
           10000730, 10000732, 15000700, 15000703,
           1034500701, 1034500710}

def msb_for_entity(eid):
    s = str(eid)
    if len(s) == 10 and s.startswith('10'):
        return f'm60_{s[2:4]}_{s[4:6]}_00'
    if len(s) == 8:
        return f'm{s[0:2]}_{s[2:4]}_00_00'
    return None

def load_paramdefs():
    defs = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType:
                defs[str(pd.ParamType)] = pd
        except Exception:
            pass
    return defs

def read_param(bnd, name, pds):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_re_p.tmp')
            from System.IO import File as SysFile
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in pds:
                p.ApplyParamdef(pds[pt])
            return p
    raise KeyError(name)

def read_msb(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_re.msb')
    from System.IO import File as SysFile
    SysFile.WriteAllBytes(tmp, data)
    return _msbe_read.Invoke(None, Array[Object]([tmp]))

def load_charaname():
    """nameId -> name from DarkScript3's er-common.CharaName.txt (= NpcName FMG)."""
    p = (config.DARKSCRIPT_RESOURCES / 'er-common.CharaName.txt') if config.DARKSCRIPT_RESOURCES \
        else Path(r'D:\tools\DarkScript3\Resources\er-common.CharaName.txt')
    m = {}
    if p.exists():
        for line in p.read_text(encoding='utf-8', errors='replace').splitlines():
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[0].isdigit():
                m[int(parts[0])] = parts[1]
    return m

def main():
    pds = load_paramdefs()
    game = config.require_game_dir()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(game / 'regulation.bin'))
    # NpcParam: id -> nameId
    np = read_param(bnd, 'NpcParam', pds)
    npc_name_id = {}
    for row in np.Rows:
        for cell in row.Cells:
            if str(cell.Def.InternalName) == 'nameId':
                try: npc_name_id[int(row.ID)] = int(str(cell.Value))
                except: pass
                break
    charaname = load_npcname_fmg(game)
    print(f"(loaded {len(charaname)} NpcName FMG entries)")
    msb_dir = game / 'map' / 'MapStudio'

    wanted_msbs = {}
    for eid in TARGETS:
        wanted_msbs.setdefault(msb_for_entity(eid), set()).add(eid)

    print(f"{'entity':>11}  {'npcParam':>8}  {'nameId':>7}  {'model':>7}  name  | partName @ map")
    print('-' * 90)
    found = set()
    for msb_base, eids in sorted(wanted_msbs.items()):
        if not msb_base:
            continue
        path = msb_dir / (msb_base + '.msb.dcx')
        if not path.exists():
            print(f"  (MSB not found: {path.name})")
            continue
        try:
            msb = read_msb(path)
        except Exception as e:
            print(f"  (read fail {path.name}: {e})")
            continue
        for e in msb.Parts.Enemies:
            ent = int(getattr(e, 'EntityID', 0) or 0)
            if ent not in eids:
                continue
            found.add(ent)
            npc = int(getattr(e, 'NPCParamID', 0) or 0)
            nid = npc_name_id.get(npc, 0)
            nm = charaname.get(nid, '???')
            model = str(e.ModelName) if hasattr(e, 'ModelName') else ''
            print(f"{ent:>11}  {npc:>8}  {nid:>7}  {model:>7}  {nm}  | {str(e.Name)} @ {msb_base}")
    missing = TARGETS - found
    if missing:
        print(f"\nNOT FOUND as placed Enemy EntityID: {sorted(missing)}")

if __name__ == '__main__':
    main()
