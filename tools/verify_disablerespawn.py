#!/usr/bin/env python3
"""Part A(a) verification — NpcParam.disableRespawn as the WorldFarmableEnemy gate.

Reads regulation.bin off disk (same table the game loads into memory at boot, so
the value is identical to the live `from::params::get_param` path) and reports
disableRespawn for known-boss vs known-trash NpcParam rows, resolved by name via
NpcName.fmg. Verifies the plan's hypothesis: 0 = respawns (farmable), 1 = one-time.

Usage: py verify_disablerespawn.py            # active profile (default ERR)
       MFG_PROFILE=vanilla py verify_disablerespawn.py
"""
import sys, io, os, tempfile, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
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
_param_read = rd('SoulsFormats.PARAM'); _bnd = rd('SoulsFormats.BND4'); _fmg = rd('SoulsFormats.FMG')
_fn_seq = 0

def frombytes(m, d, s):
    global _fn_seq; _fn_seq += 1
    t = os.path.join(tempfile.gettempdir(), '%d_dr%d%s' % (os.getpid(), _fn_seq, s))
    SysFile.WriteAllBytes(t, d.ToArray() if hasattr(d, 'ToArray') else d)
    r = m.Invoke(None, Array[Object]([t]))
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

def cell(row, names):
    """Return dict of {internalName: value} for the requested field names."""
    out = {}
    for c in row.Cells:
        n = str(c.Def.InternalName)
        if n in names:
            out[n] = c.Value
            if len(out) == len(names): break
    return out

# Known named bosses (expect disableRespawn=1) and respawning trash (expect 0).
BOSSES = ['margit', 'morgott', 'godrick', 'godefroy', 'tree sentinel', 'runebear',
          'rennala', 'radahn', 'rykard', 'malenia', 'mohg', 'maliketh', 'godfrey',
          'radagon', 'elden beast', 'astel', 'fire giant', 'rennala']
TRASH = ['soldier', 'wolf', 'rat', 'dog', 'demi-human', 'wandering noble',
         'skeleton', 'crab', 'bat', 'giant crab', 'rune bear', 'runebear',
         'godrick soldier', 'wandering nobles', 'land squirt']

def main():
    g = config.require_err_mod_dir()
    print(f'# profile={config.PROFILE}  source={g}\n')
    pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(g / 'regulation.bin'))
    npc = read_param(bnd, 'NpcParam', pds)

    # nameId -> english text from NpcName.fmg
    names = {}
    src = g
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = src / 'msg' / 'engus' / mb
        if not p.exists():
            p = config.GAME_DIR / 'msg' / 'engus' / mb   # ERR may not ship msg; fall back to vanilla
        if not p.exists(): continue
        b = frombytes(_bnd, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in b.Files:
            base = str(f.Name).replace(chr(92), '/').rsplit('/', 1)[-1]
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = frombytes(_fmg, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]': names[int(e.ID)] = t

    # scan all rows once: id, name, disableRespawn
    rows = []
    dist = {0: 0, 1: 0}
    for row in npc.Rows:
        c = cell(row, {'disableRespawn', 'nameId'})
        dr = int(str(c.get('disableRespawn', 0)))
        nid = int(str(c.get('nameId', -1)))
        nm = names.get(nid, '')
        rows.append((int(row.ID), nm, dr))
        dist[dr] = dist.get(dr, 0) + 1

    print(f'total NpcParam rows: {len(rows)}')
    print(f'disableRespawn distribution: {dist}\n')

    def show(title, needles):
        print(f'== {title} ==')
        seen = set()
        for rid, nm, dr in rows:
            low = nm.lower()
            if nm and any(n in low for n in needles):
                key = (nm, dr)
                if key in seen: continue
                seen.add(key)
                flag = 'RESPAWN(0)' if dr == 0 else ('ONE-TIME(1)' if dr == 1 else f'?({dr})')
                print(f'  {rid:>9}  dr={dr}  {flag:12}  {nm}')
        print()

    show('KNOWN BOSSES (expect dr=1)', BOSSES)
    show('KNOWN TRASH (expect dr=0)', TRASH)

    # What kind of rows actually carry dr=1? Sample the named ones.
    print('== SAMPLE of dr=1 NAMED rows (what "one-time" really contains) ==')
    n = 0
    seen = set()
    for rid, nm, dr in rows:
        if dr == 1 and nm and nm not in seen:
            seen.add(nm)
            print(f'  {rid:>9}  {nm}')
            n += 1
            if n >= 50: break
    print()

if __name__ == '__main__':
    main()
