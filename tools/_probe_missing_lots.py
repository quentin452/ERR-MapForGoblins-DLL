#!/usr/bin/env python3
"""NAME-THE-BLOCKER probe for the disk-parser-coverage-gaps chantier.

For each reported "real item missing from the map" lot, trace it through EVERY source
mechanism in the ACTUAL ERR mod files (regulation + map/MapStudio + event) and print a
verdict: where the lot lives, and therefore WHY the runtime disk parser does (or does not)
reach it. This is the offline equivalent of the runtime [SKIP-ROW]/[WATCH] diag — it lets us
distinguish "parsed-but-skipped" from "never-parsed (parser scope miss)" without the game.

Run (Windows, see [[mapforgoblins-pipeline-setup]]):
  PYTHONUTF8=1 PYTHONPATH="C:/Users/iamacat/AppData/Local/Temp/mfg_aux" MFG_PROFILE=err \
      py -3.14 tools/_probe_missing_lots.py
"""
import struct
import os
import tempfile
from pathlib import Path

import extract_all_items as E
import config

# ── The reported missing lots (memory: disk-parser-coverage-gaps) ───────────────────────
# (lot, table-hint, name)   table-hint = where the regulation row is expected to live
TARGETS = [
    (12070500,   'map',   'Ghostflame Torch (m12_07 UG)'),
    (12070320,   'map',   'Burred Bolt (m12_07 UG)'),
    (1038490030, 'map',   'Burred Bolt (m60_38_49 OW)'),
    (333062001,  'enemy', 'Larval Tear (boss)'),
    (333062011,  'enemy', 'Larval Tear (boss)'),
    (333062021,  'enemy', 'Larval Tear (boss)'),
    (333065001,  'enemy', 'Larval Tear (boss)'),
    (337000805,  'enemy', 'armor (boss)'),
]
TARGET_LOTS = {t[0] for t in TARGETS}
# Window around each target: the engine may reference a BASE and award base+k, so the literal
# target never appears. Find which nearby lot the scripts/params actually reference.
WIN_LO, WIN_HI = 64, 8
WINDOW_VALS = {v for t in TARGET_LOTS for v in range(t - WIN_LO, t + WIN_HI + 1)}
# Sibling-window: a lot may be base+k of a covered base. Scan a generous window.
SIB_WINDOW = 60
SIB_PROBE = {t[0] for t in TARGETS}

ERR_MOD_DIR = Path(config.ERR_MOD_DIR)
MSB_DIR = ERR_MOD_DIR / 'map' / 'MapStudio'
EVENT_DIR = ERR_MOD_DIR / 'event'

# Same SoulsFormats bootstrap path extract_all_items uses.
SoulsFormats = E.SoulsFormats
reg = SoulsFormats.SFUtil.DecryptERRegulation(str(ERR_MOD_DIR / 'regulation.bin'))
pdefs = E.load_paramdefs()

LOT_FIELDS = set()
for i in range(1, 9):
    LOT_FIELDS.add(f'lotItemId0{i}')
    LOT_FIELDS.add(f'lotItemCategory0{i}')
LOT_FIELDS.add('getItemFlagId')

print('=== reading ItemLotParam_map / _enemy ===')
ilp_map = E.param_to_dict(E.read_param(reg, 'ItemLotParam_map', pdefs), LOT_FIELDS)
ilp_enemy = E.param_to_dict(E.read_param(reg, 'ItemLotParam_enemy', pdefs), LOT_FIELDS)
map_bases = set(ilp_map.keys())
enemy_bases = set(ilp_enemy.keys())


def slot1(row):
    if not row:
        return None
    return (row.get('lotItemId01', 0), row.get('lotItemCategory01', 0), row.get('getItemFlagId', 0))


# ── NpcParam: which NPCs reference each target lot, and are they PLACED enemies? ─────────
print('=== reading NpcParam ===')
npc = E.param_to_dict(E.read_param(reg, 'NpcParam', pdefs), {'itemLotId_map', 'itemLotId_enemy'})
# lot -> list of (npcId, which-field)
npc_refs = {lot: [] for lot in TARGET_LOTS}
npc_window = []   # (npcId, fld, value) for any reference landing in a target window
for npc_id, f in npc.items():
    for fld in ('itemLotId_map', 'itemLotId_enemy'):
        v = f.get(fld, 0)
        if v in TARGET_LOTS:
            npc_refs[v].append((npc_id, fld))
        if v in WINDOW_VALS:
            npc_window.append((npc_id, fld, v))

# ── MSB pass: Treasure base lots + which NpcParams are actually placed (enabled) ─────────
print('=== scanning map/MapStudio (Treasures + placed enemies) ===')
_msbe_read = E._msbe_read
treasure_hits = {lot: [] for lot in TARGET_LOTS}   # lot -> [(map, part, bucket, eid, groups)]
all_treasure_bases = set()
placed_npcs = {}   # npcParamId -> (map, partName)  for enabled MSB enemies
for msb_path in sorted(MSB_DIR.glob('*.msb.dcx')):
    info = E.parse_map_name(msb_path.name)
    if not info or info['p3'] == 99:
        continue
    try:
        msb = E._read_from_bytes(_msbe_read, SoulsFormats.DCX.Decompress(str(msb_path)), '.msb')
    except Exception:
        continue
    mapn = info['map']
    # part name -> bucket ('Asset' live / 'DummyAsset' inert) + (entityId, groups) so we can tell a
    # live ground-item from an inert DummyAsset the runtime drops (msbe_parser PART_DUMMY_ASSET rule).
    part_bucket = {}
    for p in msb.Parts.Assets:
        try:
            eid = int(p.EntityID)
        except Exception:
            eid = 0
        part_bucket[str(p.Name)] = ('Asset', eid, 0)
    for p in msb.Parts.DummyAssets:
        try:
            eid = int(p.EntityID)
        except Exception:
            eid = 0
        groups = 0
        try:
            groups = sum(1 for g in (getattr(p, 'EntityGroupIDs', []) or []) if int(g) not in (0, -1))
        except Exception:
            pass
        part_bucket[str(p.Name)] = ('DummyAsset', eid, groups)
    for t in msb.Events.Treasures:
        il = int(t.ItemLotID)
        if il > 0:
            all_treasure_bases.add(il)
            if il in TARGET_LOTS:
                pn = str(t.TreasurePartName) if t.TreasurePartName else ''
                bucket, eid, groups = part_bucket.get(pn, ('<none>', 0, 0))
                treasure_hits[il].append((mapn, pn, bucket, eid, groups))
    for p in msb.Parts.Enemies:
        if int(getattr(p, 'GameEditionDisable', 0) or 0) == 1:
            continue
        nid = int(p.NPCParamID)
        if nid and nid not in placed_npcs:
            placed_npcs[nid] = (mapn, str(p.Name))

# ── EMEVD pass: full 4-byte-window scan for each target lot (catch ANY awarding event) ───
print('=== scanning event/*.emevd (broad arg-window scan) ===')
_emevd_read = E.asm.GetType('SoulsFormats.EMEVD').GetMethod(
    'Read', E.BindingFlags.Public | E.BindingFlags.Static | E.BindingFlags.FlattenHierarchy,
    None, E.Array[E.SysType]([E._str_type]), None)
TEMPLATE_EVENTS = {
    90005300, 90005301, 90005860, 90005861, 90005880, 90005750, 90005753,
    90005774, 90005792, 90005632, 90005110, 90005390, 90005555, 90005500, 90005501,
}
# lot -> list of (map, bank, instrId, invokedEventId-or-None, byteOffset, isKnownTemplate)
# Scan EVERY instruction bank, not just 2000-init: a boss's lot is often awarded by a DIRECT
# AwardItemLot (bank 2003) inside the event body, with the lot literal in the arg data — which the
# template-table parser (bank-2000 inits only) structurally cannot see.
emevd_hits = {lot: [] for lot in TARGET_LOTS}
emevd_window = []   # (value, map, bank, iid, ev_id, off) for near-target references
for emevd_path in sorted(EVENT_DIR.glob('*.emevd.dcx')):
    mapn = emevd_path.name.replace('.emevd.dcx', '')
    try:
        tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_mfg_pml.tmp')
        E.SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(emevd_path)).ToArray())
        emevd = _emevd_read.Invoke(None, E.Array[E.Object]([tmp]))
        os.unlink(tmp)
    except Exception:
        continue
    for event in emevd.Events:
        for instr in event.Instructions:
            bank = int(instr.Bank)
            iid = int(instr.ID)
            args = bytes(instr.ArgData)
            if len(args) < 4:
                continue
            ev_id = struct.unpack_from('<i', args, 4)[0] if (bank == 2000 and len(args) >= 8) else None
            nwin = len(args) // 4
            ints = [struct.unpack_from('<i', args, k * 4)[0] for k in range(nwin)]
            for k, v in enumerate(ints):
                if v in TARGET_LOTS:
                    known = (ev_id in TEMPLATE_EVENTS) if ev_id is not None else False
                    emevd_hits[v].append((mapn, bank, iid, ev_id, k * 4, known))
                elif v in WINDOW_VALS:
                    emevd_window.append((v, mapn, bank, iid, ev_id, k * 4))

# ── Sibling check: is the lot base+k of a covered base in any mechanism? ─────────────────
def sibling_of(lot):
    # A multi-item reward is a CONTIGUOUS ItemLotParam chain base..base+k; the runtime walks
    # base+1.. only from a base it already placed (treasure/emevd). So a target that is base+k of a
    # COVERED treasure base is recoverable as a sibling; one whose nearest base is just an unrelated
    # contiguous _map/_enemy row is not.
    out = []
    for k in range(1, SIB_WINDOW + 1):
        base = lot - k
        if base in all_treasure_bases:
            out.append(('covered-MSB-treasure-base', base, k))
            break
    return out


# ── Verdict ─────────────────────────────────────────────────────────────────────────────
print('\n' + '=' * 100)
print('PER-LOT BLOCKER VERDICT')
print('=' * 100)
for lot, hint, name in TARGETS:
    print(f'\n■ lot {lot}  [{hint}]  {name}')
    in_map = lot in map_bases
    in_enemy = lot in enemy_bases
    s_m = slot1(ilp_map.get(lot))
    s_e = slot1(ilp_enemy.get(lot))
    print(f'   regulation row: map={in_map} {("slot1=" + str(s_m)) if s_m else ""}'
          f'   enemy={in_enemy} {("slot1=" + str(s_e)) if s_e else ""}')

    # 1) MSB Treasure (structural — runtime build_disk_loot_markers reaches it)
    th = treasure_hits[lot]
    th_live = False
    if th:
        for mapn, pn, bucket, eid, groups in th:
            inert = (bucket == 'DummyAsset' and eid in (0, -1) and not groups)
            drop = '  ⟵ INERT DummyAsset → runtime DROPS it' if inert else ''
            if not inert:
                th_live = True
            print(f'   ✓ MSB Treasure: {mapn}/{pn}  part={bucket} eid={eid} groups={groups}{drop}')
        if th_live:
            print(f'     → build_disk_loot_markers SHOULD emit (check page/tile)')

    # 2) NpcParam → placed enemy (runtime build_disk_enemy_markers reaches it)
    nr = npc_refs[lot]
    if nr:
        for nid, fld in nr:
            placed = placed_npcs.get(nid)
            tag = f'PLACED in {placed}' if placed else 'NOT placed in any enabled MSB enemy'
            print(f'   • NpcParam {nid}.{fld} → {tag}')
        if not any(placed_npcs.get(nid) for nid, _ in nr):
            print(f'     → enemy pass CANNOT reach (boss is EMEVD-spawned / not a placed enemy part)')
    else:
        print(f'   • NpcParam: no NPC references this lot')

    # 3) EMEVD — ALL banks. (map, bank, iid, ev_id, off, known)
    eh = emevd_hits[lot]
    emevd_known_template = False
    if eh:
        seen = {}
        for mapn, bank, iid, ev_id, off, known in eh:
            seen.setdefault((bank, iid, ev_id, off, known), []).append(mapn)
        for (bank, iid, ev_id, off, known), maps in sorted(seen.items(), key=lambda x: (x[0][0], x[0][1])):
            if bank == 2000:
                ktag = 'KNOWN template' if known else 'init of UNKNOWN event (NOT in kEmevdTemplates)'
                desc = f'2000-init invoke ev={ev_id}'
            else:
                ktag = f'DIRECT instr {bank}:{iid:02d} — body award, NOT a 2000-init (parser blind)'
                desc = f'instr {bank}:{iid:02d}'
            if known:
                emevd_known_template = True
            print(f'   • EMEVD: {desc} @arg-byte {off}  [{ktag}]  maps={sorted(set(maps))[:4]}')
    else:
        print(f'   • EMEVD: lot literal never appears in ANY instruction arg (no script awards it directly)')

    # 4) Sibling of a covered base?
    sib = sibling_of(lot)
    if sib:
        print(f'   • sequence-sibling of: {sib}')

    # 5) Window scan: what NEARBY lot do the scripts/params actually reference? (computed base+k)
    win_npc = [(nid, fld, v) for (nid, fld, v) in npc_window if lot - WIN_LO <= v <= lot + WIN_HI]
    win_evd = [(v, mapn, bank, iid, ev_id, off) for (v, mapn, bank, iid, ev_id, off) in emevd_window
               if lot - WIN_LO <= v <= lot + WIN_HI]
    win_tre = [b for b in all_treasure_bases if lot - WIN_LO <= b <= lot + WIN_HI]
    if win_npc or win_evd or win_tre:
        print(f'   ⌖ NEARBY references (the lot may be a computed base+k of one of these):')
        for nid, fld, v in win_npc:
            placed = placed_npcs.get(nid)
            ptag = f'PLACED in {placed}' if placed else 'NOT placed'
            print(f'       NpcParam {nid}.{fld} = {v}   (Δ={lot - v:+d})  [{ptag}]')
        for v, mapn, bank, iid, ev_id, off in sorted(set(win_evd)):
            ev = f' invoke ev={ev_id}' if ev_id is not None else ''
            print(f'       EMEVD {mapn} instr {bank}:{iid:02d} arg@{off} = {v}{ev}   (Δ={lot - v:+d})')
        for b in sorted(win_tre):
            print(f'       MSB-treasure-base = {b}   (Δ={lot - b:+d})')

    # Net verdict
    npc_placed = any(placed_npcs.get(nid) for nid, _ in nr)
    if not th and not nr and not eh and not sib:
        print('   ✗✗ VERDICT: lot has NO source in MSB/NpcParam/EMEVD/sibling — bake-only or wrong lot id')
    elif th_live or npc_placed or emevd_known_template:
        print('   ~ VERDICT: a runtime pass SHOULD reach it → check skip filters (flag/dedup) or page/tile')
    else:
        print('   ✗ VERDICT: PARSER SCOPE MISS — source exists but no runtime pass reaches it '
              '(inert dummy / EMEVD-spawned boss / non-template or direct-body event)')

print('\nDone.')
