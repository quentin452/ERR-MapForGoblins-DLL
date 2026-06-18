#!/usr/bin/env python3
"""Map NPC name -> placements (entity, TalkID, map) so decompiled t<TalkID>.esd
files can be labelled by NPC. Usage: py _npc_talkids.py Leda Ansbach ..."""
import sys, io, os, tempfile, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from collections import defaultdict
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
_param, _bnd4, _fmg, _msbe = rd('SoulsFormats.PARAM'), rd('SoulsFormats.BND4'), rd('SoulsFormats.FMG'), rd('SoulsFormats.MSBE')
def frombytes(m, d, s):
    t = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_nt' + s)
    SysFile.WriteAllBytes(t, d.ToArray() if hasattr(d, 'ToArray') else d)
    r = m.Invoke(None, Array[Object]([t])); os.unlink(t); return r
DEFAULT = ['Leda', 'Ansbach', 'Moore', 'Thiollier', 'Dane', 'Freyja', 'Hornsent',
           'Queelign', 'Igon', 'Jolan', 'Patches']
def load_pds():
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
            p = frombytes(_param, f.Bytes, '.param')
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in pds: p.ApplyParamdef(pds[pt])
            return p
    raise KeyError(name)
def main():
    needles = [a.lower() for a in sys.argv[1:]] or [d.lower() for d in DEFAULT]
    g = config.require_game_dir(); pds = load_pds()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(g / 'regulation.bin'))
    np = read_param(bnd, 'NpcParam', pds)
    nid = {}
    for row in np.Rows:
        for c in row.Cells:
            if str(c.Def.InternalName) == 'nameId':
                try: nid[int(row.ID)] = int(str(c.Value))
                except: pass
                break
    names = {}
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = g / 'msg' / 'engus' / mb
        if not p.exists(): continue
        b = frombytes(_bnd4, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in b.Files:
            base = str(f.Name).replace(chr(92), '/').rsplit('/', 1)[-1]
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = frombytes(_fmg, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]': names[int(e.ID)] = t
    hits = defaultdict(set)
    for path in sorted((g / 'map' / 'MapStudio').glob('*.msb.dcx')):
        try: msb = frombytes(_msbe, SoulsFormats.DCX.Decompress(str(path)), '.msb')
        except Exception: continue
        mp = path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            ent = int(getattr(e, 'EntityID', 0) or 0)
            if ent <= 0: continue
            nm = names.get(nid.get(int(getattr(e, 'NPCParamID', 0) or 0), 0), '')
            if nm and any(n in nm.lower() for n in needles):
                hits[nm].add((ent, int(getattr(e, 'TalkID', 0) or 0), mp))
    for nm in sorted(hits):
        print(f"\n{nm}")
        for ent, tid, mp in sorted(hits[nm]):
            print(f"    entity {ent}  talkId {tid}  ({mp})  -> t{tid}.py" if tid else f"    entity {ent}  (no talkId)  ({mp})")
if __name__ == '__main__':
    main()
