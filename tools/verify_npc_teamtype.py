#!/usr/bin/env python3
"""Tier 3 verification — NpcParam teamType/npcType -> MapGenie NPC categories.

Reads NpcParam off regulation.bin (disk == live) and reports, per teamType (and
npcType), how many NAMED NpcParam rows fall there plus sample names — so the six
MapGenie NPC categories (Character, Ghost, Merchant, Trainer, Elite Enemy, Enemy)
can be read off against real teamType values. Anchor: the shipped code already uses
teamType @ +0x133, teamType in {24,27} = named invader (goblin_inject.hpp:360).

Usage: py verify_npc_teamtype.py            # ERR (default)
       MFG_PROFILE=vanilla py verify_npc_teamtype.py
"""
import sys, io, os, tempfile, re, collections
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
_seq = 0
def frombytes(m, d, s):
    global _seq; _seq += 1
    t = os.path.join(tempfile.gettempdir(), '%d_tt%d%s' % (os.getpid(), _seq, s))
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

def main():
    g = config.require_err_mod_dir()
    print(f'# profile={config.PROFILE}  source={g}\n')
    pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(str(g / 'regulation.bin'))
    npc = read_param(bnd, 'NpcParam', pds)

    names = {}
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = g / 'msg' / 'engus' / mb
        if not p.exists(): p = config.GAME_DIR / 'msg' / 'engus' / mb
        if not p.exists(): continue
        b = frombytes(_bnd, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in b.Files:
            base = str(f.Name).replace(chr(92), '/').rsplit('/', 1)[-1]
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = frombytes(_fmg, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]': names[int(e.ID)] = t

    def want(row, fields):
        out = {}
        for c in row.Cells:
            n = str(c.Def.InternalName)
            if n in fields:
                out[n] = int(str(c.Value))
                if len(out) == len(fields): break
        return out

    rows = []
    for row in npc.Rows:
        c = want(row, {'teamType', 'npcType', 'nameId', 'disableRespawn'})
        nm = names.get(c.get('nameId', -1), '')
        rows.append((int(row.ID), nm, c.get('teamType', -1), c.get('npcType', -1), c.get('disableRespawn', 0)))

    named = [r for r in rows if r[1]]
    print(f'total rows={len(rows)}  named rows={len(named)}\n')

    # npcType distribution (all rows + named) — is it actually populated?
    nt_all = collections.Counter(r[3] for r in rows)
    nt_named = collections.Counter(r[3] for r in named)
    print(f'npcType distribution  all={dict(nt_all)}  named={dict(nt_named)}')
    inv = sorted({r[0] for r in named if r[2] in (24, 27)})
    print(f'named rows with teamType in {{24,27}} (shipped invader gate): {len(inv)}\n')

    # teamType distribution over NAMED rows (named = has an NpcName entry = a "real" NPC/enemy)
    by_team = collections.defaultdict(list)
    for rid, nm, tt, nt, dr in named:
        by_team[tt].append((nm, nt, dr, rid))
    print(f"{'teamType':>8} {'#named':>6}  distinct names (sample up to 14)")
    print('-' * 110)
    for tt in sorted(by_team):
        lst = by_team[tt]
        uniq = []
        seen = set()
        for nm, nt, dr, rid in lst:
            if nm not in seen:
                seen.add(nm); uniq.append(nm)
        print(f'{tt:>8} {len(seen):>6}  {", ".join(uniq[:14])}')
    print()

    # what are the npcType==1 rows? (the paramdef "trash/boss" flag)
    print('== npcType==1 named rows (paramdef: distinguishes trash/boss) ==')
    seen = set(); n1 = []
    t1 = collections.Counter()
    for rid, nm, tt, nt, dr in named:
        if nt == 1:
            t1[tt] += 1
            if nm not in seen:
                seen.add(nm); n1.append((tt, nm))
    print(f'   distinct named={len(seen)}   teamType among them={dict(t1)}')
    print('   sample:', ', '.join(f'{nm}[t{tt}]' for tt, nm in n1[:24]))
    print()

    # cross-check known NPCs of each MapGenie kind
    KNOWN = {
        'Merchant': ['kal', 'nomadic merchant', 'twin maiden', 'patches'],
        'Character(friendly quest)': ['roderika', 'gideon', 'gowry', 'boc', 'millicent', 'gostoc', 'iji'],
        'Ghost/Invader': ['vyke', 'okina', 'nerijus', 'henricus', 'alberich', 'yura', 'magnus'],
        'Elite/Field boss': ['tree sentinel', 'night', 'deathbird', 'crucible knight', 'death rite bird'],
    }
    for title, needles in KNOWN.items():
        print(f'== {title} ==')
        seen = set()
        for rid, nm, tt, nt, dr in named:
            low = nm.lower()
            if any(n in low for n in needles) and (nm, tt) not in seen:
                seen.add((nm, tt))
                print(f'   team={tt:<3} npcType={nt:<3} dr={dr}  {nm}')
        print()

if __name__ == '__main__':
    main()
