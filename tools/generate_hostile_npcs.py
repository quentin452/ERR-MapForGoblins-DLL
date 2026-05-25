#!/usr/bin/env python3
"""
Generate World - Hostile NPC.MASSEDIT — fully auto-discovered.

Strategy:
  1. From NpcParam (regulation.bin), collect NPC IDs with teamType in
     {24, 27} — both are hostile-invader variants used in vanilla and ERR.
  2. Scan all MSBs for Enemies whose NPCParamID is in that set AND
     whose EntityID > 0 (placed, not script-spawned dummies).
  3. Cross-reference items_database (built by extract_all_items.py) by
     (map, partName) to attach:
       - defeatFlag (from template 90005792 X0_4) for clearedEventFlagId
         and textDisableFlagId (hide-on-kill behaviour)
       - main drop item for textId1 fallback when NPC has no NpcName entry
  4. Each matched enemy becomes a marker labeled with the NPC's name
     (NpcParam.nameId + 700000000 → NpcName FMG via runtime patcher).
"""
import sys, io, os, tempfile, json
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')
import config
from collections import defaultdict
from pathlib import Path
from pythonnet import load
load('coreclr')
import clr
clr.AddReference(str(config.SOULSFORMATS_DLL))
from System.Reflection import Assembly, BindingFlags
from System import Array, Type as SysType, Object
from System.IO import File as SysFile
import SoulsFormats

from massedit_common import (OUT_DIR, DATA_DIR, UNDERGROUND_AREAS, DLC_AREAS,
                             OVERWORLD_AREAS, get_disp_mask, resolve_location_id_at)

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
_param_read = asm.GetType('SoulsFormats.PARAM').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)
_msbe_read = asm.GetType('SoulsFormats.MSBE').GetMethod('Read',
    BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy,
    None, Array[SysType]([_str_type]), None)


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


def read_param(bnd, name, paramdefs):
    for f in bnd.Files:
        if name in str(f.Name):
            tmp = os.path.join(tempfile.gettempdir(), '_hnp_p.tmp')
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in paramdefs:
                p.ApplyParamdef(paramdefs[pt])
            return p
    return None


def read_msb(path):
    tmp = os.path.join(tempfile.gettempdir(), '_hnp_m.tmp')
    SysFile.WriteAllBytes(tmp, SoulsFormats.DCX.Decompress(str(path)).ToArray())
    m = _msbe_read.Invoke(None, Array[Object]([tmp]))
    os.unlink(tmp)
    return m


def map_to_area(map_name):
    parts = map_name.replace('m', '').split('_')
    try: return int(parts[0]), int(parts[1]), int(parts[2])
    except: return 0, 0, 0


# Both teams used by hostile NPC invaders in ER + ERR:
#   24 = vanilla invader (Edgar, Vyke, ...)
#   27 = ERR / DLC hostile-NPC variant (Millicent, ...)
INVADER_TEAM_TYPES = {24, 27}


def main():
    print('Loading NpcParam (invader filter, NPC names)...')
    pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(
        str(config.require_err_mod_dir() / 'regulation.bin'))
    np = read_param(bnd, 'NpcParam', pds)

    # Map NpcParam ID -> (teamType, nameId)
    npc_info = {}
    invader_ids = set()
    for row in np.Rows:
        team = name_id = None
        for cell in row.Cells:
            n = str(cell.Def.InternalName)
            if n == 'teamType':
                try: team = int(str(cell.Value))
                except: pass
            elif n == 'nameId':
                try: name_id = int(str(cell.Value))
                except: pass
        npc_info[int(row.ID)] = (team, name_id)
        if team in INVADER_TEAM_TYPES:
            invader_ids.add(int(row.ID))
    print(f'  {len(invader_ids)} NpcParam IDs with teamType in {INVADER_TEAM_TYPES}')

    # Index items_database by (map, partName) → list of records (for drops + defeat flag)
    items_db_path = DATA_DIR / 'items_database.json'
    db_by_part = defaultdict(list)
    if items_db_path.exists():
        with open(items_db_path, encoding='utf-8') as f:
            for entry in json.load(f):
                if not isinstance(entry, dict): continue
                key = (entry.get('map'), entry.get('partName'))
                if key[0] and key[1]:
                    db_by_part[key].append(entry)

    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'
    records = []
    print(f'Scanning MSBs for invader placements...')
    for msb_path in sorted(msb_dir.glob('*.msb.dcx')):
        try: msb = read_msb(msb_path)
        except Exception: continue
        map_name = msb_path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            if int(getattr(e, 'GameEditionDisable', 0) or 0) == 1:
                continue
            npc = int(getattr(e, 'NPCParamID', 0) or 0)
            if npc not in invader_ids: continue
            entity = int(getattr(e, 'EntityID', 0) or 0)
            if entity <= 0:
                continue  # runtime-only dummy
            # Filter out mob enemies that happen to share teamType 24/27
            # (Bloodfiends c4280, dungeon Battlemages c4300_*_28 variants,
            # scarabs c4190/91/92, etc). Real NPC invaders all have a
            # named NpcName entry — `nameId > 0` is the canonical signal.
            _team, name_id = npc_info.get(npc, (None, None))
            if not name_id or name_id <= 0:
                continue
            part_name = str(e.Name)
            pos = e.Position
            area, gx, gz = map_to_area(map_name)

            # Lookup drops + defeat flag from items_database
            db_entries = db_by_part.get((map_name, part_name), [])
            defeat_flag = 0
            for db_e in db_entries:
                if db_e.get('defeatFlag', 0) > 0:
                    defeat_flag = int(db_e['defeatFlag'])
                    break

            records.append({
                'entity': entity, 'npc': npc, 'nameId': name_id,
                'map': map_name, 'area': area, 'gx': gx, 'gz': gz,
                'x': float(pos.X), 'y': float(pos.Y), 'z': float(pos.Z),
                'model': str(e.ModelName) if hasattr(e, 'ModelName') else '',
                'defeatFlag': defeat_flag, 'partName': part_name,
            })

    # Dedup per (map, rounded_coords) — multiple invader variants stacked
    # at the same EMEVD trigger spot would otherwise produce overlapping
    # markers. Keep the first (which is usually the canonical placement).
    seen = set()
    uniq = []
    for r in records:
        key = (r['map'], round(r['x'], 1), round(r['z'], 1))
        if key in seen: continue
        seen.add(key)
        uniq.append(r)
    records = uniq
    records.sort(key=lambda r: (r['area'], r['gx'], r['gz'], r['x'], r['z']))

    lines = []
    row_id = 9200000
    named = 0
    flagged = 0
    for r in records:
        disp = get_disp_mask(r['area'])
        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = 374;')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {r["area"]};')
        if r['area'] in OVERWORLD_AREAS or r['area'] in DLC_AREAS or r['gx'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {r["gx"]};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {r["gz"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {r["x"]:.3f};')
        if r['y'] != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {r["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {r["z"]:.3f};')

        # textId1: NPC name via NpcName FMG (resolved at runtime by
        # goblin_messages.cpp using the +700000000 offset convention).
        if r['nameId'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {r["nameId"] + 700000000};')
            named += 1
            # textDisableFlagId1: hide NPC name once defeated
            if r['defeatFlag'] > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId1: = {r["defeatFlag"]};')

        # textId2: location subtitle (interior maps only)
        loc_id = resolve_location_id_at(r['map'], r['x'], r['y'], r['z'])
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
            if r['defeatFlag'] > 0:
                lines.append(f'param WorldMapPointParam: id {row_id}: textDisableFlagId2: = {r["defeatFlag"]};')

        # clearedEventFlagId: shows green checkmark when defeated. C++
        # config (hideKilledBosses) chooses between checkmark and full hide.
        if r['defeatFlag'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: clearedEventFlagId: = {r["defeatFlag"]};')
            flagged += 1

        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out = OUT_DIR / 'World - Hostile NPC.MASSEDIT'
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Written {len(records)} hostile NPC markers ({named} with name, {flagged} with defeat flag) to {out.name}')


if __name__ == '__main__':
    main()
