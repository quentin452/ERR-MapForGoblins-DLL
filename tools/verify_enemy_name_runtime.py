#!/usr/bin/env python3
"""Offline feasibility test for baked_data_full_removal_plan Phase 1 (enemy names).

The non-ERR builds bake data/enemy_names_i18n.json into PlaceName because the
enemy-drop marker looks its name up at `enemyId + 900000000` (decode_textid routes
+900M -> TutorialTitle FMG). Question: does the ACTIVE install's OWN runtime files
already resolve those ids? If yes, the bake is redundant and removable; if no, the
runtime replacement needs a different source (or the bake stays).

Checks each baked enemy id against, in the given profile's install:
  (a) menu.msgbnd TutorialTitle  (the FMG decode_textid +900M points at)
  (b) NpcName via model->NpcParam.nameId bridge (the mod-agnostic alternative)

Usage: MFG_PROFILE=vanilla py verify_enemy_name_runtime.py
"""
import sys, io, os, tempfile, json, re
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
_seq = 0
def rd(method, data, suffix):
    global _seq; _seq += 1
    t = os.path.join(tempfile.gettempdir(), '%d_en%d%s' % (os.getpid(), _seq, suffix))
    SysFile.WriteAllBytes(t, data.ToArray() if hasattr(data, 'ToArray') else data)
    r = method.Invoke(None, Array[Object]([t]))
    try: os.unlink(t)
    except OSError: pass
    return r

def tutorialtitle_ids(install):
    """Set of ids present in the install's menu.msgbnd TutorialTitle (engus)."""
    for name in ('menu_dlc02.msgbnd.dcx', 'menu.msgbnd.dcx'):
        mp = install / 'msg' / 'engus' / name
        if mp.exists(): break
    else:
        return {}
    bnd = rd(_br, SoulsFormats.DCX.Decompress(str(mp)), '.bnd')
    out = {}
    for f in bnd.Files:
        if not os.path.basename(str(f.Name)).startswith('TutorialTitle'):
            continue
        fmg = rd(_fr, f.Bytes, '.fmg')
        for e in fmg.Entries:
            t = str(e.Text) if e.Text else ''
            if t and t != '[ERROR]':
                out[int(e.ID)] = re.sub(r'^\d+[a-z]?\.\s*', '', t).strip()
    return out

def main():
    baked = json.load(open(config.PROJECT_DIR / 'data' / 'enemy_names_i18n.json', encoding='utf-8'))
    baked_ids = {int(k): v.get('engus', '') for k, v in baked.items()}
    print(f'# profile={config.PROFILE}')
    print(f'baked enemy-name ids (data/enemy_names_i18n.json): {len(baked_ids)}\n')

    install = config.require_err_mod_dir()   # the active profile's install
    tt = tutorialtitle_ids(install)
    tt_enemy = {i for i in tt if i in baked_ids}
    print(f'(a) TutorialTitle in {config.PROFILE} install: {len(tt)} total entries')
    print(f'    baked ids ALSO present in this install TutorialTitle: {len(tt_enemy)} / {len(baked_ids)}')
    missing = sorted(set(baked_ids) - set(tt))
    print(f'    baked ids MISSING from this install TutorialTitle: {len(missing)}')
    if missing[:8]:
        print('    e.g.', ', '.join(f'{i}={baked_ids[i]}' for i in missing[:8]))
    # spot: do the present ones match text?
    mism = [(i, baked_ids[i], tt[i]) for i in sorted(tt_enemy) if baked_ids[i] and tt[i] != baked_ids[i]][:6]
    if mism:
        print('    text mismatches (baked vs install):')
        for i, b, t in mism: print(f'      {i}: baked={b!r} install={t!r}')
    print()
    verdict = 'REDUNDANT (install resolves them) -> bake removable' if len(missing) == 0 \
        else f'BAKE STILL NEEDED for {len(missing)} ids (install TutorialTitle lacks them)'
    print(f'VERDICT (a) [{config.PROFILE}]: {verdict}\n')

    # (b) mod-agnostic bridge: enemyId(model*1000+variant*100+4) -> NpcParam(model*10000+variant*1000)
    #     -> nameId -> NpcName FMG. Every install has NpcName, so if this resolves, the runtime path
    #     is model->NpcParam->NpcName (no bake, no TutorialTitle dependency).
    _pr = asm.GetType('SoulsFormats.PARAM').BaseType.GetMethod('Read', Array[SysType]([_str]))
    def load_pd():
        d = {}
        for x in config.PARAMDEF_DIR.glob('*.xml'):
            try:
                pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
                if pd and pd.ParamType: d[str(pd.ParamType)] = pd
            except Exception: pass
        return d
    pds = load_pd()
    reg = SoulsFormats.SFUtil.DecryptERRegulation(str(install / 'regulation.bin'))
    npc = None
    for f in reg.Files:
        if 'NpcParam' in str(f.Name):
            npc = rd(_pr, f.Bytes, '.param')
            if str(npc.ParamType) in pds: npc.ApplyParamdef(pds[str(npc.ParamType)])
            break
    nameid_of = {}
    for row in npc.Rows:
        for c in row.Cells:
            if str(c.Def.InternalName) == 'nameId':
                nameid_of[int(row.ID)] = int(str(c.Value)); break
    # NpcName fmg (vanilla lives in item.msgbnd)
    names = {}
    for mb in ('item.msgbnd.dcx', 'item_dlc01.msgbnd.dcx', 'item_dlc02.msgbnd.dcx'):
        p = install / 'msg' / 'engus' / mb
        if not p.exists(): p = config.GAME_DIR / 'msg' / 'engus' / mb
        if not p.exists(): continue
        b = rd(_br, SoulsFormats.DCX.Decompress(str(p)), '.bnd')
        for f in b.Files:
            base = os.path.basename(str(f.Name).replace('\\', '/'))
            if base == 'NpcName.fmg' or re.match(r'NpcName_dlc\d+\.fmg$', base, re.I):
                fmg = rd(_fr, f.Bytes, '.fmg')
                for e in fmg.Entries:
                    t = str(e.Text) if e.Text else ''
                    if t and t != '[ERROR]': names[int(e.ID)] = t
    resolved = miss_np = miss_nm = 0
    examples = []
    for eid, bname in baked_ids.items():
        model = eid // 1000; variant = (eid % 1000) // 100
        npid = model * 10000 + variant * 1000
        nid = nameid_of.get(npid)
        if nid is None: miss_np += 1; continue
        rn = names.get(nid)
        if not rn: miss_nm += 1; continue
        resolved += 1
        if len(examples) < 6: examples.append((eid, bname, rn))
    print(f'(b) model->NpcParam({{model*10000+variant*1000}})->nameId->NpcName in {config.PROFILE}:')
    print(f'    resolved: {resolved}/{len(baked_ids)}   (no NpcParam row: {miss_np}, row but no name: {miss_nm})')
    for eid, b, r in examples:
        print(f'      {eid}: baked={b!r}  runtime={r!r}  {"OK" if b==r else "DIFF"}')

if __name__ == '__main__':
    main()
