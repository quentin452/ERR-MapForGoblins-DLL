#!/usr/bin/env python3
"""Measure the PROGRESSION-LOCK GRAPH of the loaded game, derived from its own files.

The randomizer-scope question (docs/plans/runtime_randomizer_scope.md): can a reachability
graph be DERIVED from whatever mod is loaded, or does it need a curated logic table (which
LEGAL + the mod-agnostic doctrine both forbid)? A graph is derivable if it is FLAT — few
distinct items gating anything. This counts them, from EMEVD only, no bake, no table.

Signal = the EMEVD instruction "IF Player Has/Doesn't Have Item" (+ the BBox variant): the
engine's own "is this locked for you?" test. Every key-item door, medallion lift and rune
gate goes through it.

Run from tools/ (cwd needs oo2core):
  MFG_PROFILE=err py -3 _probe_progression_gates.py [--profile err|vanilla|convergence]
"""
import sys, io, os, json, struct, tempfile
from collections import Counter, defaultdict
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
import extract_all_items as E

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_emevd_read = asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


def load_emevd(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_gate.tmp')
    SysFile.WriteAllBytes(tmp, data)
    e = _emevd_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return e


# ── EMEDF: locate the possession-test instructions + decode their args ──
EMEDF = json.load(open(config.TOOLS_DIR / 'er-common.emedf.json', encoding='utf-8'))
TARGETS = {}   # (bank, index) -> (name, argdefs)
for mc in EMEDF['main_classes']:
    for ins in mc['instrs']:
        if ins['name'].startswith("IF Player Has/Doesn't Have Item"):
            TARGETS[(mc['index'], ins['index'])] = (ins['name'], ins['args'])

SIZE = {0: 1, 1: 2, 2: 4, 3: 1, 4: 2, 5: 4, 6: 4, 8: 4}
FMT = {0: 'B', 1: 'H', 2: 'I', 3: 'b', 4: 'h', 5: 'i', 6: 'f', 8: 'I'}


def decode(argdefs, blob):
    """EMEVD packs args back-to-back, each aligned to its own width."""
    off, out, spans = 0, [], []
    for a in argdefs:
        sz = SIZE[a['type']]
        off = (off + sz - 1) // sz * sz
        if off + sz > len(blob):
            return None, None
        out.append(struct.unpack_from('<' + FMT[a['type']], blob, off)[0])
        spans.append((off, sz))
        off += sz
    return out, spans


# Item Type enum (EMEDF "Item Type"), used to pick the right name FMG.
ITEM_TYPE = {0: 'Weapon', 1: 'Protector', 2: 'Accessory', 3: 'Goods', 4: 'Gem'}


def main():
    src = config.require_err_mod_dir()      # profile-aware (err / vanilla / convergence)
    event_dir = Path(src) / 'event'
    files = sorted(event_dir.glob('*.emevd.dcx'))
    print(f'profile={config.PROFILE}  src={src}')
    print(f'scanning {len(files)} EMEVDs for {len(TARGETS)} possession-test instructions...')

    hits = Counter()                 # (itemType, itemId) -> occurrences
    maps_of = defaultdict(set)       # gate -> {map}
    total_inst = 0
    # Item ID supplied by the CALLER (the generic "locked door needing key X" templates live
    # here, so skipping them would miss the real traversal locks). Resolved after the scan
    # against every InitializeEvent / InitializeCommonEvent arg blob.
    pending = []                     # {key, src} awaiting a caller
    callers = defaultdict(list)      # (map|'__common__', event_id) -> [initializer arg blob]

    for p in files:
        try:
            emevd = load_emevd(p)
        except Exception:
            continue
        map_name = p.name.replace('.emevd.dcx', '')
        for evt in emevd.Events:
            evt_id = int(evt.ID)
            # byte ranges of this event's instructions that are filled by the CALLER
            par = defaultdict(list)
            for prm in evt.Parameters:
                par[int(prm.InstructionIndex)].append(
                    (int(prm.TargetStartByte), int(prm.ByteCount), int(prm.SourceStartByte)))
            for i, inst in enumerate(evt.Instructions):
                key = (int(inst.Bank), int(inst.ID))
                # collect every initializer so parameterized gates can be resolved
                if key in ((2000, 0), (2000, 6)):
                    ab = bytes(inst.ArgData) if inst.ArgData else b''
                    if len(ab) >= 8:
                        callee = struct.unpack_from('<I', ab, 4)[0]
                        ck = ('__common__' if key == (2000, 6) else map_name, callee)
                        callers[ck].append(ab)
                    continue
                if key not in TARGETS:
                    continue
                total_inst += 1
                name, argdefs = TARGETS[key]
                blob = bytes(inst.ArgData) if inst.ArgData else b''
                vals, spans = decode(argdefs, blob)
                if vals is None:
                    continue
                # arg order: [cond group, Item Type, Item ID, Desired State]
                itype, iid = vals[1], vals[2]
                ioff, isz = spans[2]
                srcs = [src for s, n, src in par.get(i, ())
                        if ioff < s + n and s < ioff + isz]
                if srcs:
                    pending.append({'map': map_name, 'event': evt_id,
                                    'src': srcs[0], 'type': itype})
                    continue
                if iid <= 0:
                    continue
                hits[(itype, iid)] += 1
                maps_of[(itype, iid)].add(map_name)

    # ── resolve the caller-supplied ids: initializer args are [slot, eventId, params...],
    # so a parameter's SourceStartByte is relative to byte 8 of the initializer's blob.
    resolved = unresolved = 0
    for g in pending:
        blobs = callers.get((g['map'], g['event']), [])
        if g['map'] == 'common_func' or not blobs:
            blobs = blobs + callers.get(('__common__', g['event']), [])
        got = False
        for ab in blobs:
            off = 8 + g['src']
            if off + 4 > len(ab):
                continue
            iid = struct.unpack_from('<I', ab, off)[0]
            if iid <= 0 or iid > 2_000_000_000:
                continue
            hits[(g['type'], iid)] += 1
            maps_of[(g['type'], iid)].add(g['map'])
            got = True
        resolved += got
        unresolved += (not got)

    # ── names, from the mod's own msgbnd ──
    # NB: BND4.Read(path) keeps the file mapped, so E._read_from_bytes' unlink throws
    # WinError 5 on a .bnd. Write our own temp and leave it for the OS to reap.
    _t = os.path.join(tempfile.gettempdir(), f'{os.getpid()}_mfg_gate_msg.bnd')
    SysFile.WriteAllBytes(_t, SoulsFormats.DCX.Decompress(str(E.MSGBND_PATH)).ToArray())
    msgbnd = E._bnd4_read.Invoke(None, Array[Object]([_t]))
    fmg = {}
    for t, fam in ((0, 'WeaponName'), (1, 'ProtectorName'), (2, 'AccessoryName'),
                   (3, 'GoodsName'), (4, 'GemName')):
        try:
            fmg[t] = E.read_fmg_names(msgbnd, fam + '.fmg')
        except Exception:
            fmg[t] = {}

    print(f'\npossession-test instructions found : {total_inst}')
    print(f'  ...caller-supplied item id, resolved via the initializer : {resolved}')
    print(f'  ...caller-supplied, no caller found (dead/nested template) : {unresolved}')
    print(f'DISTINCT ITEMS THAT GATE ANYTHING  : {len(hits)}   <-- the graph-flatness number')
    by_type = Counter(t for t, _ in hits)
    print('  by item type: ' + ', '.join(f'{ITEM_TYPE.get(t, t)}={n}' for t, n in by_type.items()))

    rows = []
    for (t, iid), n in hits.most_common():
        nm = fmg.get(t, {}).get(iid, '')
        rows.append({'type': ITEM_TYPE.get(t, t), 'id': iid, 'name': nm,
                     'tests': n, 'maps': len(maps_of[(t, iid)])})
    # ── refine: a possession test is only a PROGRESSION lock if the item is a KEY ITEM.
    # goodsType is in the loaded regulation, so this stays derived-from-the-game.
    reg = SoulsFormats.SFUtil.DecryptERRegulation(str(Path(src) / 'regulation.bin'))
    goods = E.param_to_dict(E.read_param(reg, 'EquipParamGoods', E.load_paramdefs()),
                            {'goodsType'})
    gtype = {int(k): int(v['goodsType']) for k, v in goods.items()}

    by_gt = Counter()
    keys = []
    for (t, iid), n in hits.items():
        if t != 3:            # Goods only; weapons/armour are never progression keys
            continue
        gt = gtype.get(iid)
        by_gt[gt] += 1
        if gt == 1:           # 1 = Key Item
            keys.append((iid, n, fmg.get(3, {}).get(iid, ''), len(maps_of[(t, iid)])))
    print('\nGoods gates by goodsType (1 = Key Item): '
          + ', '.join(f'{g}:{c}' for g, c in sorted(by_gt.items(), key=lambda x: -x[1])))
    print(f'★ KEY ITEMS that gate anything: {len(keys)}')
    print(f'\n{"key item":<44} {"id":>9} {"tests":>6} {"maps":>5}')
    for iid, n, nm, nm_maps in sorted(keys, key=lambda r: -r[1]):
        print(f'{(nm or "?"):<44} {iid:>9} {n:>6} {nm_maps:>5}')

    # ── the OTHER lock class EMEVD misses: ObjAct doors/lifts whose requirement is in the
    # param itself (spQualifiedType 1 = item, 2 = event flag). Derived from the regulation.
    oa = E.param_to_dict(E.read_param(reg, 'ObjActParam', E.load_paramdefs()),
                         {'spQualifiedType', 'spQualifiedId', 'spQualifiedType2',
                          'spQualifiedId2', 'spQualifiedPassEventFlag'})
    oa_by_type = Counter()
    oa_items, oa_flags = set(), set()
    for rid, r in oa.items():
        for tf, idf in (('spQualifiedType', 'spQualifiedId'),
                        ('spQualifiedType2', 'spQualifiedId2')):
            t, v = int(r[tf]), int(r[idf])
            if t == 0 and v == 0:
                continue
            oa_by_type[t] += 1
            (oa_items if t == 1 else oa_flags).add(v)
    print(f'\nObjActParam rows: {len(oa)}; qualified slots by spQualifiedType: '
          + ', '.join(f'{t}:{n}' for t, n in sorted(oa_by_type.items())))
    print(f'  distinct ITEM requirements (type 1): {len(oa_items)} -> {sorted(oa_items)[:20]}')
    print(f'  distinct FLAG requirements (type 2): {len(oa_flags)}')

    out = Path(tempfile.gettempdir()) / f'progression_gates_{config.PROFILE}.json'
    out.write_text(json.dumps(rows, indent=1, ensure_ascii=False), encoding='utf-8')
    print(f'\nfull list -> {out}')


if __name__ == '__main__':
    main()
