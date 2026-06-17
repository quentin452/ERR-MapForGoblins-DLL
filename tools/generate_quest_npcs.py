#!/usr/bin/env python3
"""
Generate World - Quest NPC.MASSEDIT — named friendly NPCs + merchants.

The friendly sibling of generate_hostile_npcs.py. Where that one filters
NpcParam teamType in {24,27} (invaders), this one keeps the FRIENDLY/quest
NPCs: the named characters and merchants the player navigates to for quest
progress and shopping. v1 = LOCATIONS only (where each NPC first stands),
NOT curated quest-steps (no game-data source for those).

Strategy:
  1. From NpcParam: collect NPC IDs whose teamType is in FRIENDLY_TEAM_TYPES
     AND that have a real NpcName (nameId > 0). The name requirement is the
     key signal — generic ambient creatures have no NpcName, named friendly
     characters and merchants do.
  2. Scan all MSBs for Enemies (NPCs are placed as MSB "Enemies" regardless of
     team) whose NPCParamID is in that set AND EntityID > 0 (placed, not
     script-spawned dummies).
  3. Each placement becomes a marker labelled with the NPC's name
     (nameId + 700000000 → NpcName FMG via the runtime patcher).

NO defeat/clear flag (friendly NPCs are not killed for completion). A future
v2 could hide an NPC once its questline ends, but that needs per-NPC EMEVD
curation — out of scope for v1.

⚠ TUNE ON WINDOWS: FRIENDLY_TEAM_TYPES below is a best-guess. The friendly-NPC
teamType taxonomy must be verified against real data. Run:
    python tools/generate_quest_npcs.py --inspect
to dump teamType -> count + sample (nameId, model, partName); set
FRIENDLY_TEAM_TYPES to the teamTypes whose samples are clearly friendly
NPCs/merchants (Kalé, Twin Maiden Husks, Roderika, etc.) and exclude any that
turn out to be enemies/bosses, THEN run without --inspect to emit the MASSEDIT.
"""
import sys, io, os, tempfile
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
import SoulsFormats

from massedit_common import (OUT_DIR, OVERWORLD_AREAS, DLC_AREAS,
                             get_disp_mask, resolve_location_id_at)

# ── TUNABLE: friendly / quest-NPC teamTypes (verify via --inspect) ───────────
# Best-guess defaults; MUST be confirmed against NpcParam data on Windows.
# Invader teamTypes {24,27} are handled by generate_hostile_npcs.py and are
# intentionally NOT here.
FRIENDLY_TEAM_TYPES = {1, 2, 6, 7, 8}

# Worldmap icon sprite for the quest-NPC family. 370=grace, 374=hostile NPC.
# TODO(visual): pick a distinct friendly/quest icon from the worldmap atlas and
# confirm in-game; 374 is a safe-renders placeholder so the layer is visible.
ICON_ID = 374

asm = Assembly.LoadFrom(str(config.SOULSFORMATS_DLL))
_str_type = SysType.GetType('System.String')
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
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_qnp_p.tmp')
            from System.IO import File as SysFile
            SysFile.WriteAllBytes(tmp, f.Bytes)
            p = SoulsFormats.PARAM.Read(tmp)
            p.ApplyParamdef(paramdefs[name])
            return p
    raise KeyError(name)


def read_msb(path):
    data = SoulsFormats.DCX.Decompress(str(path)).ToArray()
    tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_qnp.msb')
    from System.IO import File as SysFile
    SysFile.WriteAllBytes(tmp, data)
    return _msbe_read.Invoke(None, Array[Object]([tmp]))


def map_to_area(map_name):
    parts = map_name.replace('m', '').split('_')
    area = int(parts[0])
    gx = int(parts[1]) if len(parts) > 1 else 0
    gz = int(parts[2]) if len(parts) > 2 else 0
    return area, gx, gz


def read_npc_info(bnd, pds):
    """NpcParam ID -> (teamType, nameId)."""
    np = read_param(bnd, 'NpcParam', pds)
    info = {}
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
        info[int(row.ID)] = (team, name_id)
    return info


def inspect(npc_info, msb_dir):
    """Dump teamType -> placed-named-NPC samples so the friendly set can be tuned."""
    # which NPC ids are actually placed (EntityID>0) and named
    placed = defaultdict(list)  # team -> [(npc, nameId, model, part, map)]
    for msb_path in sorted(msb_dir.glob('*.msb.dcx')):
        try: msb = read_msb(msb_path)
        except Exception: continue
        map_name = msb_path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            npc = int(getattr(e, 'NPCParamID', 0) or 0)
            if int(getattr(e, 'EntityID', 0) or 0) <= 0:
                continue
            team, name_id = npc_info.get(npc, (None, None))
            if not name_id or name_id <= 0:
                continue
            model = str(e.ModelName) if hasattr(e, 'ModelName') else ''
            placed[team].append((npc, name_id, model, str(e.Name), map_name))
    print('teamType -> placed named-NPC count (sample: npc/nameId/model/part@map)')
    for team in sorted(placed, key=lambda t: -len(placed[t])):
        s = placed[team]
        print(f'  team {team}: {len(s)} placements')
        for npc, nid, model, part, mp in s[:8]:
            print(f'      npc={npc} nameId={nid} model={model} {part}@{mp}')


def main():
    pds = load_paramdefs()
    bnd = SoulsFormats.SFUtil.DecryptERRegulation(
        str(config.require_err_mod_dir() / 'regulation.bin'))
    npc_info = read_npc_info(bnd, pds)
    msb_dir = config.require_err_mod_dir() / 'map' / 'MapStudio'

    if '--inspect' in sys.argv:
        inspect(npc_info, msb_dir)
        return

    friendly_ids = {nid for nid, (team, name) in npc_info.items()
                    if team in FRIENDLY_TEAM_TYPES and name and name > 0}
    print(f'{len(friendly_ids)} friendly named NpcParam IDs (teamType in {sorted(FRIENDLY_TEAM_TYPES)})')

    records = []
    for msb_path in sorted(msb_dir.glob('*.msb.dcx')):
        try: msb = read_msb(msb_path)
        except Exception: continue
        map_name = msb_path.name.replace('.msb.dcx', '')
        for e in msb.Parts.Enemies:
            if int(getattr(e, 'GameEditionDisable', 0) or 0) == 1:
                continue
            npc = int(getattr(e, 'NPCParamID', 0) or 0)
            if npc not in friendly_ids:
                continue
            entity = int(getattr(e, 'EntityID', 0) or 0)
            if entity <= 0:
                continue
            _team, name_id = npc_info.get(npc, (None, None))
            pos = e.Position
            area, gx, gz = map_to_area(map_name)
            records.append({
                'entity': entity, 'npc': npc, 'nameId': name_id,
                'map': map_name, 'area': area, 'gx': gx, 'gz': gz,
                'x': float(pos.X), 'y': float(pos.Y), 'z': float(pos.Z),
                'partName': str(e.Name),
            })

    # Dedup per (map, rounded coords): the same NPC re-placed at one spot
    # across MSB variants would double-mark.
    seen, uniq = set(), []
    for r in records:
        key = (r['map'], round(r['x'], 1), round(r['z'], 1))
        if key in seen: continue
        seen.add(key)
        uniq.append(r)
    records = uniq
    records.sort(key=lambda r: (r['area'], r['gx'], r['gz'], r['x'], r['z']))

    lines = []
    row_id = 9300000
    named = 0
    for r in records:
        disp = get_disp_mask(r['area'])
        lines.append(f'param WorldMapPointParam: id {row_id}: iconId: = {ICON_ID};')
        lines.append(f'param WorldMapPointParam: id {row_id}: {disp}: = 1;')
        lines.append(f'param WorldMapPointParam: id {row_id}: areaNo: = {r["area"]};')
        if r['area'] in OVERWORLD_AREAS or r['area'] in DLC_AREAS or r['gx'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: gridXNo: = {r["gx"]};')
            lines.append(f'param WorldMapPointParam: id {row_id}: gridZNo: = {r["gz"]};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posX: = {r["x"]:.3f};')
        if r['y'] != 0.0:
            lines.append(f'param WorldMapPointParam: id {row_id}: posY: = {r["y"]:.3f};')
        lines.append(f'param WorldMapPointParam: id {row_id}: posZ: = {r["z"]:.3f};')
        if r['nameId'] and r['nameId'] > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId1: = {r["nameId"] + 700000000};')
            named += 1
        loc_id = resolve_location_id_at(r['map'], r['x'], r['y'], r['z'])
        if loc_id > 0:
            lines.append(f'param WorldMapPointParam: id {row_id}: textId2: = {loc_id};')
        lines.append(f'param WorldMapPointParam: id {row_id}: selectMinZoomStep: = 1;')
        row_id += 1

    out = OUT_DIR / 'World - Quest NPC.MASSEDIT'
    with open(out, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'Written {len(records)} quest-NPC markers ({named} named) to {out.name}')


if __name__ == '__main__':
    main()
