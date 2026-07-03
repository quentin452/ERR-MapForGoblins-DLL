#!/usr/bin/env python3
"""Offline RE probe for docs/re/windows_enemy_name_runtime_source_re_prompt.md.

Question: is there ANY install-resident data path from a live enemy model
(c3251 Tree Sentinel / c4311 / c4600 / c6060 / c6100) to a localized display
name, with no hand-authored table? Runs against BOTH the vanilla install
(GAME_DIR) and the ERR overlay (ERR_MOD_DIR, vanilla fallback per me3
semantics) and reports:

  [S4] full NpcName FMG dump stats + model-derived id-formula probes +
       generic-name text searches (prompt section 4)
  [S2] where "Tree Sentinel" / "Field Boss" strings live across ALL engus
       FMGs, incl. the fmg32 id 5763010 banner entry (prompt section 2,
       offline half)
  [S1] every regulation param field matching /(name|text|msg|caption)Id/i,
       probed at model-derived row ids, nonzero values resolved across all
       FMGs (prompt section 1)
  [NPC] NpcParam.nameId spot-check for the test models, vanilla vs ERR

Usage (cwd=tools so oo2core resolves):  py -3.14 probe_enemy_name_sources.py
"""
import sys, io, os, re, tempfile
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from pythonnet import load; load('coreclr')
from System.Reflection import Assembly
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
import SoulsFormats

_str = SysType.GetType('System.String')
_fr = asm.GetType('SoulsFormats.FMG').BaseType.GetMethod('Read', Array[SysType]([_str]))
_br = asm.GetType('SoulsFormats.BND4').BaseType.GetMethod('Read', Array[SysType]([_str]))
_pr = asm.GetType('SoulsFormats.PARAM').BaseType.GetMethod('Read', Array[SysType]([_str]))
_seq = 0
def rd(method, data, suffix):
    global _seq; _seq += 1
    t = os.path.join(tempfile.gettempdir(), '%d_ens%d%s' % (os.getpid(), _seq, suffix))
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = method.Invoke(None, Array[Object]([t]))
    try: os.unlink(t)
    except OSError: pass
    return r

MODELS = [3251, 4311, 4600, 6060, 6100]   # field boss, humanoids, beasts (Linux session set)
NPC_IDS = {3251: 32510010}                # live-observed npcParamId for c3251 (ERR)

MSGBNDS = ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx',
           'menu.msgbnd.dcx', 'menu_dlc01.msgbnd.dcx', 'menu_dlc02.msgbnd.dcx')

def load_fmgs(install, fallback):
    """{fmg_basename: {id: text}} from all engus msgbnds; overlay file wins, dlc merges over base."""
    out = {}
    for name in MSGBNDS:
        p = install / 'msg' / 'engus' / name
        if not p.exists() and fallback is not None:
            p = fallback / 'msg' / 'engus' / name
        if not p.exists():
            continue
        bnd = rd(_br, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in bnd.Files:
            base = os.path.basename(str(f.Name).replace('\\', '/'))
            if not base.lower().endswith('.fmg'):
                continue
            key = re.sub(r'_dlc\d+', '', base[:-4])   # merge dlc FMGs into the base name
            fmg = rd(_fr, f.Bytes, '.fmg')
            d = out.setdefault(key, {})
            for e in fmg.Entries:
                t = str(e.Text) if e.Text else ''
                if t and t != '[ERROR]':
                    d[int(e.ID)] = t
    return out

def load_params(install, fallback):
    """{param_name: (paramdef, {row_id: row})} for params whose def has a name-ish field."""
    pds = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType: pds[str(pd.ParamType)] = pd
        except Exception: pass
    regp = install / 'regulation.bin'
    if not regp.exists() and fallback is not None:
        regp = fallback / 'regulation.bin'
    reg = SoulsFormats.SFUtil.DecryptERRegulation(str(regp))
    out = {}
    for f in reg.Files:
        base = os.path.basename(str(f.Name).replace('\\', '/'))
        if not base.lower().endswith('.param'):
            continue
        try:
            pm = rd(_pr, f.Bytes, '.param')
        except Exception:
            continue
        pt = str(pm.ParamType)
        if pt not in pds:
            continue
        try:
            pm.ApplyParamdef(pds[pt])
        except Exception:
            continue
        out[base[:-6]] = (pds[pt], pm)
    return out

NAMEISH = re.compile(r'(name|text|msg|caption|title)[_]?(id)?$', re.I)

def nameish_fields(pd):
    """[(index, internal_name)] of fields whose name smells like a text id."""
    res = []
    for i, fd in enumerate(pd.Fields):
        n = str(fd.InternalName)
        if NAMEISH.search(n) and 'sfx' not in n.lower():
            res.append((i, n))
    return res

def row_candidates(param, model, npcid):
    """Row ids in this param that are plausibly derived from the model number."""
    ids = {model, model * 10, model * 100, model * 1000, model * 10000, npcid}
    if npcid:
        ids |= {npcid // 10, npcid // 100, npcid // 1000}
    have = set()
    rows = {}
    for r in param.Rows:
        rid = int(r.ID)
        if rid in ids or rid // 10000 == model or rid // 1000 == model:
            rows[rid] = r
    return rows

def resolve_everywhere(fmgs, val):
    """[(fmg, text)] entries whose id == val, across all loaded FMGs."""
    hits = []
    for name, d in fmgs.items():
        if val in d:
            hits.append((name, d[val]))
    return hits

def section4(tag, fmgs):
    npc = fmgs.get('NpcName', {})
    print(f'\n[S4:{tag}] NpcName: {len(npc)} entries; id range '
          f'{min(npc) if npc else 0}..{max(npc) if npc else 0}')
    # histogram by id band (10^4 bands are enough to see the layout)
    bands = {}
    for i in npc: bands[i // 100000] = bands.get(i // 100000, 0) + 1
    print(f'[S4:{tag}] id bands (x100000): ' +
          ' '.join(f'{k}:{v}' for k, v in sorted(bands.items())))
    for needle in ('Tree Sentinel', 'Sentinel', 'Soldier', 'Wolf', 'Godrick Soldier', 'Warhawk'):
        hits = [(i, t) for i, t in npc.items() if needle.lower() in t.lower()]
        show = ', '.join(f'{i}={t!r}' for i, t in sorted(hits)[:6])
        print(f'[S4:{tag}] NpcName contains {needle!r}: {len(hits)}  {show}')
    for m in MODELS:
        npcid = NPC_IDS.get(m, m * 10000 + 10)
        cands = {m, m * 10, m * 100, m * 1000, m * 10000, npcid,
                 npcid // 10, npcid // 100, npcid // 1000}
        hits = {i: npc[i] for i in cands if i in npc}
        print(f'[S4:{tag}] c{m}: model-derived NpcName ids hit: '
              f'{hits if hits else "NONE"}')

def section2(tag, fmgs):
    print(f'\n[S2:{tag}] searching all {len(fmgs)} FMGs for banner strings')
    for needle in ('Tree Sentinel', 'Field Boss'):
        cnt = 0
        for name, d in sorted(fmgs.items()):
            for i, t in d.items():
                if needle.lower() in t.lower():
                    cnt += 1
                    if cnt <= 12:
                        print(f'[S2:{tag}]  {needle!r} -> {name} id={i}: {t[:90]!r}')
        print(f'[S2:{tag}]  {needle!r}: {cnt} total hits')
    hits = resolve_everywhere(fmgs, 5763010)
    print(f'[S2:{tag}] id 5763010 present in: ' +
          ('; '.join(f'{n}: {t[:90]!r}' for n, t in hits) if hits else 'NOWHERE'))

def section1(tag, fmgs, params):
    print(f'\n[S1:{tag}] params with name-ish fields, probed at model-derived rows')
    for pname in sorted(params):
        pd, pm = params[pname]
        flds = nameish_fields(pd)
        if not flds:
            continue
        any_hit = False
        for m in MODELS:
            rows = row_candidates(pm, m, NPC_IDS.get(m, m * 10000 + 10))
            for rid, r in sorted(rows.items()):
                for i, fn in flds:
                    try:
                        v = int(str(r.Cells[i].Value))
                    except (ValueError, IndexError):
                        continue
                    if v <= 0:
                        continue
                    res = resolve_everywhere(fmgs, v)
                    rs = '; '.join(f'{n}={t[:60]!r}' for n, t in res[:4]) if res else 'no FMG hit'
                    print(f'[S1:{tag}]  {pname}[{rid}].{fn} = {v}  ({rs})')
                    any_hit = True
        if not any_hit:
            names = ','.join(fn for _, fn in flds)
            print(f'[S1:{tag}]  {pname}: fields[{names}] -> all zero / no model-derived rows')

def npc_namecheck(tag, fmgs, params):
    print(f'\n[NPC:{tag}] NpcParam.nameId for test models (all rows in the model band)')
    if 'NpcParam' not in params:
        print(f'[NPC:{tag}] NpcParam missing!'); return
    pd, pm = params['NpcParam']
    idx = next((i for i, fd in enumerate(pd.Fields) if str(fd.InternalName) == 'nameId'), None)
    npcname = fmgs.get('NpcName', {})
    total = nonzero = resolves = 0
    band = set(MODELS)
    for r in pm.Rows:
        rid = int(r.ID)
        total += 1
        v = int(str(r.Cells[idx].Value))
        if v > 0:
            nonzero += 1
            if v in npcname: resolves += 1
        if rid // 10000 in band:
            res = npcname.get(v, '') if v > 0 else ''
            print(f'[NPC:{tag}]  NpcParam[{rid}].nameId = {v}  {res!r}')
    print(f'[NPC:{tag}] rows total={total}, nameId!=0: {nonzero} (of which resolve in NpcName: {resolves})')

def run(tag, install, fallback):
    print(f'\n===== install [{tag}] = {install} (fallback={fallback}) =====')
    fmgs = load_fmgs(install, fallback)
    print(f'[{tag}] FMGs loaded: {len(fmgs)} ({sum(len(d) for d in fmgs.values())} entries)')
    section4(tag, fmgs)
    section2(tag, fmgs)
    params = load_params(install, fallback)
    print(f'\n[{tag}] params with paramdef applied: {len(params)}')
    npc_namecheck(tag, fmgs, params)
    section1(tag, fmgs, params)

def main():
    game = config.GAME_DIR
    err = config.ERR_MOD_DIR
    run('vanilla', game, None)
    if err and err != game:
        run('err', err, game)

if __name__ == '__main__':
    main()
