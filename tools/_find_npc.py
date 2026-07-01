#!/usr/bin/env python3
"""Find placed NPC entities by NAME substring across MSBs. Temp helper for the
EMEVD death-flag entity bridge. Usage: py _find_npc.py Iji Jerren Gostoc"""
import sys, io, os, tempfile, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pathlib import Path
from pythonnet import load; load('coreclr')
import clr; clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
def rd(tn):
    return asm.GetType(tn).GetMethod('Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
_param_read = rd('SoulsFormats.PARAM'); _bnd = rd('SoulsFormats.BND4')
_fmg = rd('SoulsFormats.FMG'); _msbe = rd('SoulsFormats.MSBE')
_fn_seq = 0
def frombytes(m, d, s):
    # Unique name PER CALL: SoulsFormats keeps the temp file memory-mapped for the
    # process lifetime, so a per-process-fixed name collides on the 2nd read.
    global _fn_seq; _fn_seq += 1
    t = os.path.join(tempfile.gettempdir(), '%d_fn%d%s' % (os.getpid(), _fn_seq, s))
    SysFile.WriteAllBytes(t, d.ToArray() if hasattr(d, 'ToArray') else d)
    r = m.Invoke(None, Array[Object]([t]))
    # The mapping stays open, so unlink throws even though the read succeeded;
    # best-effort cleanup, the OS reaps the temp dir anyway.
    try: os.unlink(t)
    except OSError: pass
    return r
def load_paramdefs():
    d = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType: d[str(pd.ParamType)] = pd
        except Exception: pass
    return d
def read_param(bnd, name, pds):
    for f in bnd.Files:
        if name in str(f.Name):
            p = frombytes(_param_read, f.Bytes, '.param')
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in pds: p.ApplyParamdef(pds[pt])
            return p
    raise KeyError(name)
def main():
    needles = [a.lower() for a in sys.argv[1:]] or ['iji', 'jerren', 'gostoc']
    g = config.require_game_dir(); pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(g / 'regulation.bin'))
    np = read_param(bnd, 'NpcParam', pds)
    npc_name_id = {}
    for row in np.Rows:
        for c in row.Cells:
            if str(c.Def.InternalName) == 'nameId':
                try: npc_name_id[int(row.ID)] = int(str(c.Value))
                except: pass
                break
    names = {}
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = g / 'msg' / 'engus' / mb
        if not p.exists(): continue
        b = frombytes(_bnd, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in b.Files:
            base = str(f.Name).replace(chr(92), '/').rsplit('/', 1)[-1]
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = frombytes(_fmg, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]': names[int(e.ID)] = t
    # target npcParam ids whose name matches a needle
    want = {nid: nm for npc, nid in npc_name_id.items() for nm in [names.get(nid, '')]
            if nm and any(n in nm.lower() for n in needles)}
    print('matching nameIds:', {k: v for k, v in want.items()})
    msb_dir = g / 'map' / 'MapStudio'
    hits = []
    for path in sorted(msb_dir.glob('*.msb.dcx')):
        try: msb = frombytes(_msbe, SoulsFormats.DCX.Decompress(str(path)), '.msb')
        except Exception: continue
        mp = path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            ent = int(getattr(e, 'EntityID', 0) or 0)
            if ent <= 0: continue
            npc = int(getattr(e, 'NPCParamID', 0) or 0)
            nid = npc_name_id.get(npc, 0); nm = names.get(nid, '')
            if nm and any(n in nm.lower() for n in needles):
                model = str(e.ModelName) if hasattr(e, 'ModelName') else ''
                hits.append((nm, ent, npc, model, str(e.Name), mp))
    print(f"\n{'name':28} {'entity':>11} {'npcParam':>9} {'model':>7}  part @ map")
    print('-' * 90)
    for nm, ent, npc, model, part, mp in sorted(hits):
        print(f"{nm:28} {ent:>11} {npc:>9} {model:>7}  {part} @ {mp}")

if __name__ == '__main__':
    main()
