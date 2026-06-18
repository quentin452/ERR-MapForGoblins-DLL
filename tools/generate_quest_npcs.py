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

# ── friendly / quest-NPC teamTypes (tuned via --inspect on ERR data) ─────────
# teamType is the placement's COMBAT state, not a per-character trait: one quest
# NPC has placements across several teams (friendly phase, turns-hostile phase,
# invades-you phase). So no team is purely friendly. These four are the ones
# DOMINATED by named merchants + questline NPCs (Gideon, Twin Maiden Husks, Hewg,
# Nomadic/Hermit/Isolated Merchants, Leda, Ansbach, Roderika, Blaidd, …). The
# enemy/boss/invader-dominated teams (6=field bosses/mimics, 7=great-enemy bosses,
# 27/24=invaders, 48/33/9/52=misc enemies) are intentionally excluded. Residual
# enemy leakage in these four is minor and tolerable for v1.
FRIENDLY_TEAM_TYPES = {0, 1, 2, 26}

# Non-character models to drop: c1000 is the menu/system object model (685 "Menu"
# rows + "Spirit Monument"/"Trial of Recollection"/… map-event objects, mostly in
# team 0). They carry an NpcName but are NOT NPCs you navigate to. FORCE_NAME_IDS
# (below) are exempted — a couple of real merchants legitimately use c1000.
EXCLUDE_MODELS = {'c1000'}

# ── Filter audit fixes (docs/windows_npc_filter_prompt.md) ───────────────────
# teamType + c1000 alone leaves false positives (map-event objects placed under a
# NON-c1000 model) and misses static merchants (wrong team, or c1000 model). All
# nameIds below verified against live NpcParam/MSB (tools/_qnp_diag.py).

# Objects / enemies that leak through the friendly teams. Dropped by nameId
# regardless of team or model.
DENY_NAME_IDS = {
    170000,                  # Altar of Anticipation (8 leak via model c4300)
    160100, 160200, 160500,  # Church / Smithing Table / Cathedral of Dragon Communion (c0100)
    133200,                  # no NpcName FMG entry -> renders blank (unnamed c0000)
    110100,                  # Torrent (the mount, not a navigable NPC)
    120000,                  # Rennala (boss; bosses are a separate layer)
}
DENY_NAME_IDS |= set(range(121601, 121611))   # "Menu"/"Lord's Journey" menu family (some on c0000)

# Static "resident" merchants whose placement teamType (8/27) or c1000 model would
# otherwise drop them. Force-included by nameId regardless of team AND model.
FORCE_NAME_IDS = {
    160000,            # Twin Maiden Husks (Roundtable bell-bearing merchant; team 0, model c1000)
    121800, 121810,    # Asimi, Silver Tear / Eternal King (Carian Manor; team 8)
    135200,            # Preceptor Miriam (Shaded Castle sorcery merchant; team 27)
    140601,            # Ancient Dragon Florissax (Dragon Communion; DLC)
}


def _is_generator_enemy_name(nid):
    """9-digit nameIds (>= 900000000) are the generator-name enemy encoding (e.g.
    Equilibrious Rat 904080600); real NPCs/merchants are 6-digit (<= ~189999)."""
    return nid >= 900000000

# Worldmap icon for the quest-NPC family — a dedicated friendly glyph synthesised
# by build_vanilla_gfx (3rd appended frame, after anon=441 + cluster=442). The
# quest-NPC layer is ERR-only (offset 0), so iconId = 443 directly.
ICON_ID = 443

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
            tmp = os.path.join(tempfile.gettempdir(), str(os.getpid()) + '_qnp_p.tmp')
            from System.IO import File as SysFile
            SysFile.WriteAllBytes(tmp, f.Bytes.ToArray())
            p = _param_read.Invoke(None, Array[Object]([tmp]))
            os.unlink(tmp)
            pt = str(p.ParamType) if p.ParamType else ''
            if pt in paramdefs:
                p.ApplyParamdef(paramdefs[pt])
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
                    if name and name > 0
                    and name not in DENY_NAME_IDS
                    and not _is_generator_enemy_name(name)
                    and (team in FRIENDLY_TEAM_TYPES or name in FORCE_NAME_IDS)}
    print(f'{len(friendly_ids)} friendly named NpcParam IDs '
          f'(teamType in {sorted(FRIENDLY_TEAM_TYPES)} + {len(FORCE_NAME_IDS)} forced, '
          f'minus denylist)')

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
            model = str(e.ModelName) if hasattr(e, 'ModelName') else ''
            if model in EXCLUDE_MODELS and name_id not in FORCE_NAME_IDS:
                continue   # drop c1000 menu/system objects (but keep forced merchants)
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
    # WorldMapPointParam row-id base. Must NOT collide with other layers:
    # loot 9000000/9100000, hostile-NPC 9200000, hero-tomb-statues 9300000.
    # (9300000 was the original value and clobbered all 16 hero-tomb rows at bake
    # time — generate_data keys rows by id, last writer wins.) 9400000 is free.
    row_id = 9400000
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
