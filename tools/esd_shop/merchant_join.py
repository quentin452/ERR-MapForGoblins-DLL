#!/usr/bin/env python3
"""Merchant-pin join (RE spike for merchant_item_search_plan.md Slice 3).

Proves the full offline join now that the EzState arg decoder works
(docs/re/esd_ezstate_decoder_re_findings.md):

    t<TalkID>.esd  ── talk cmd 1:22 OpenRegularShop(shopBegin,shopEnd) ──►  shop-id range
    MSB Enemy part ── TalkID ──►  (EntityID, world Position, NPCParamID→name)
    join on TalkID  ──►  merchant = (name, entity, map, position, shop range)

Emits one row per placed merchant. This is the data a Slice-3 marker layer needs
(position + shop range → the existing ShopLineupParam→items index fills the stock).

Reads: ERR regulation NpcParam + NpcName FMG (names), the VANILLA game MSBs (NPC
placements — ERR keeps vanilla map geometry), and the ERR talkesdbnd via esd_shop.
Needs pythonnet + Andre.SoulsFormats; run from repo root. The OpenRegularShop id is
1:22 (RE'd 2026-07-07: every 1:22 arg pair is a real ShopLineupParam range).
"""
import sys, os, re, io, tempfile, subprocess, collections
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
sys.path.insert(0, 'tools')
import config
from pythonnet import load; load('coreclr')
import clr; clr.AddReference(str(config.SOULSFORMATS_DLL))
import SoulsFormats
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile

ERR_ROOT = os.environ.get('ERR_ROOT') or r"D:/DOWNLOAD/ERRv2.2.9.6-541-2-2-9-6-1780861369/ERRv2.2.9.6"
ERR_REG = os.path.join(ERR_ROOT, 'mod', 'regulation.bin')
TALKDIR = os.path.join(ERR_ROOT, 'mod', 'script', 'talk')
ESD_EXE = ['dotnet', os.path.join('tools', 'esd_shop', 'bin_net9', 'esd_shop.dll')]
OPEN_SHOP = '1:22'          # OpenRegularShop(shopBegin, shopEnd) — RE'd

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str = SysType.GetType('System.String')
def rd(tn): return asm.GetType(tn).GetMethod('Read', BindingFlags.Public|BindingFlags.Static|BindingFlags.FlattenHierarchy, None, Array[SysType]([_str]), None)
_param, _bnd4, _fmg, _msbe = rd('SoulsFormats.PARAM'), rd('SoulsFormats.BND4'), rd('SoulsFormats.FMG'), rd('SoulsFormats.MSBE')
_tmpn = [0]
def frombytes(m, d, s):
    _tmpn[0] += 1
    t = os.path.join(tempfile.gettempdir(), f"{os.getpid()}_mj{_tmpn[0]}{s}")
    SysFile.WriteAllBytes(t, d.ToArray() if hasattr(d, 'ToArray') else d)
    r = m.Invoke(None, Array[Object]([t]))
    try: os.unlink(t)
    except OSError: pass
    return r


def shop_ranges_by_talkid():
    """{TalkID:int -> [(begin,end), ...]} from every 1:22 across the ERR talk ESDs."""
    out = collections.defaultdict(list)
    for fn in sorted(os.listdir(TALKDIR)):
        if not fn.endswith('.talkesdbnd.dcx'):
            continue
        dump = subprocess.run(ESD_EXE + [os.path.join(TALKDIR, fn), 'dump', OPEN_SHOP],
                              capture_output=True, text=True).stdout
        for ln in dump.splitlines():
            m = re.search(r't(\d+)\s+1:22\s+args=\[(-?\d+),\s*(-?\d+)\]', ln)
            if m:
                out[int(m.group(1))].append((int(m.group(2)), int(m.group(3))))
    return out


def npc_names():
    """{NpcParamID -> display name} via ERR NpcParam.nameId + vanilla NpcName FMG."""
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(ERR_REG)
    pds = {}
    for x in config.PARAMDEF_DIR.glob('*.xml'):
        try:
            pd = SoulsFormats.PARAMDEF.XmlDeserialize(str(x), False)
            if pd and pd.ParamType: pds[str(pd.ParamType)] = pd
        except Exception: pass
    npc = None
    for f in bnd.Files:
        if 'NpcParam' in str(f.Name) and 'Think' not in str(f.Name):
            npc = frombytes(_param, f.Bytes, '.param')
            if npc.ParamType and str(npc.ParamType) in pds: npc.ApplyParamdef(pds[str(npc.ParamType)])
            break
    nameid = {}
    for row in npc.Rows:
        for c in row.Cells:
            if str(c.Def.InternalName) == 'nameId':
                try: nameid[int(row.ID)] = int(str(c.Value))
                except: pass
                break
    g = config.require_game_dir()
    txt = {}
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
                    if t and t != '[ERROR]': txt[int(e.ID)] = t
    return {pid: txt.get(nameid.get(pid, 0), '') for pid in nameid}


def main():
    ranges = shop_ranges_by_talkid()
    print(f"[esd] {len(ranges)} TalkIDs run OpenRegularShop (1:22)")
    names = npc_names()
    g = config.require_game_dir()
    merchants = []
    for path in sorted((g / 'map' / 'MapStudio').glob('*.msb.dcx')):
        try: msb = frombytes(_msbe, SoulsFormats.DCX.Decompress(str(path)), '.msb')
        except Exception: continue
        mp = path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            tid = int(getattr(e, 'TalkID', 0) or 0)
            if tid not in ranges: continue
            ent = int(getattr(e, 'EntityID', 0) or 0)
            pos = getattr(e, 'Position', None)
            nm = names.get(int(getattr(e, 'NPCParamID', 0) or 0), '') or '?'
            merchants.append((nm, ent, tid, mp,
                              (round(pos.X, 1), round(pos.Y, 1), round(pos.Z, 1)) if pos else None,
                              ranges[tid]))
    # talkId 1000 = the DLC scaling "dummy" (a fake multi-shop on invisible props), not a real
    # merchant → drop it. Dedup by (name, shopRange, tile-without-LOD): the overworld places the
    # same NPC on _00 and _10 LOD tiles at identical coords.
    def tile_key(mp): return re.sub(r'_\d0$', '', mp)   # m60_44_39_10 -> m60_44_39
    uniq = {}
    for nm, ent, tid, mp, pos, rngs in merchants:
        if tid == 1000: continue
        k = (nm, tuple(rngs), tile_key(mp), pos)
        uniq.setdefault(k, (nm, ent, tid, mp, pos, rngs))
    rows = sorted(uniq.values())
    print(f"[msb] {len(merchants)} placements -> {len(rows)} real merchants (dummy+LOD-dup filtered):\n")
    for nm, ent, tid, mp, pos, rngs in rows:
        rs = ' '.join(f"{a}-{b}" for a, b in rngs)
        print(f"  {nm:28} entity={ent:<12} talk={tid:<11} {mp:16} pos={pos}  shops[{rs}]")

    if '--json' in sys.argv:
        import json
        out = [{'name': nm, 'entity': ent, 'talkId': tid, 'map': mp,
                'pos': list(pos) if pos else None, 'shops': rngs} for nm, ent, tid, mp, pos, rngs in rows]
        with open('tools/esd_shop/merchants.json', 'w', encoding='utf-8') as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
        print(f"\n[json] wrote tools/esd_shop/merchants.json ({len(out)} merchants)")


if __name__ == '__main__':
    main()
