#!/usr/bin/env python3
"""
Generate C++ source files from MASSEDIT data.

Parses all .MASSEDIT files to create WorldMapPointParam entries;
also emits dungeon→overworld conversion table from WorldMapLegacyConvParam.

Output:
  - src/generated/goblin_map_data.cpp  (param entries + category enum)
  - src/generated/goblin_legacy_conv.hpp (dungeon coord conversion)

Localization is handled by the DLL at runtime via FMG offset-encoding
(textId = real_id + category_offset), so no text compilation step is needed.
"""

import os
import re
import json
import sys
from collections import defaultdict
from pathlib import Path

# Category mapping: MASSEDIT filename prefix -> Category enum
CATEGORY_MAP = {
    "Equipment - Armaments": "EquipArmaments",
    "Equipment - Armour": "EquipArmour",
    "Equipment - Ashes of War": "EquipAshesOfWar",
    "Equipment - Spirits": "EquipSpirits",
    "Equipment - Talismans": "EquipTalismans",
    "Key - Celestial Dew": "KeyCelestialDew",
    "Key - Cookbooks": "KeyCookbooks",
    "Key - Crystal Tears": "KeyCrystalTears",
    "Key - Great Runes": "KeyGreatRunes",
    "Key - Imbued Sword Keys": "KeyImbuedSwordKeys",
    "Key - Larval Tears": "KeyLarvalTears",
    "Key - Lost Ashes": "KeyLostAshes",
    "Key - Pots n Perfumes": "KeyPotsNPerfumes",
    "Key - Scadutree Fragments": "KeyScadutreeFragments",
    "Key - Seeds Tears Ashes": "KeySeedsTears",
    "Key - Whetblades": "KeyWhetblades",
    "Loot - Ammo": "LootAmmo",
    "Loot - Bell-Bearings": "LootBellBearings",
    "Loot - Consumables": "LootConsumables",
    "Loot - Crafting Materials": "LootCraftingMaterials",
    "Loot - Gestures": "LootGestures",
    "Loot - Gloveworts": "LootGloveworts",
    "Loot - Golden Runes": "LootGoldenRunes",
    "Loot - Golden Runes (Low)": "LootGoldenRunesLow",
    "Loot - Great Gloveworts": "LootGreatGloveworts",
    "Loot - Greases": "LootGreases",
    "Loot - Material Nodes": "LootMaterialNodes",
    "Loot - Merchant Bell-Bearings": "LootMerchantBellBearings",
    "Loot - MP-Fingers": "LootMPFingers",
    "Loot - Prattling Pates": "LootPrattlingPates",
    "Loot - Rada Fruit": "LootRadaFruit",
    "Loot - Reusables": "LootReusables",
    "Loot - Smithing Stones": "LootSmithingStones",
    "Loot - Smithing Stones (Low)": "LootSmithingStonesLow",
    "Loot - Smithing Stones (Rare)": "LootSmithingStonesRare",
    "Loot - Stat Boosts": "LootStatBoosts",
    "Loot - Stonesword Keys": "LootStoneswordKeys",
    "Loot - Throwables": "LootThrowables",
    "Loot - Rune Arcs": "LootRuneArcs",
    "Loot - Dragon Hearts": "LootDragonHearts",
    "Loot - Utilities": "LootUtilities",
    "Magic - Incantations": "MagicIncantations",
    "Magic - Memory Stones": "MagicMemoryStones",
    "Magic - Prayerbooks": "MagicPrayerbooks",
    "Magic - Sorceries": "MagicSorceries",
    "Quest - Deathroot": "QuestDeathroot",
    "Quest - Progression": "QuestProgression",
    "Quest - Seedbed Curses": "QuestSeedbedCurses",
    "Reforged - Ember Pieces": "ReforgedEmberPieces",
    "Reforged - Fortunes": "ReforgedFortunes",
    "Reforged - Items": "ReforgedItemsAndChanges",
    "Reforged - Rune Pieces": "ReforgedRunePieces",
    "Reforged - Sealed Curios": "ReforgedItemsAndChanges",
    "World - Bosses": "WorldBosses",
    "World - Graces": "WorldGraces",
    "World - Hostile NPC": "WorldHostileNPC",
    "World - Quest NPC": "WorldQuestNPC",
    "World - Imp Statues": "WorldImpStatues",
    "World - Maps": "WorldMaps",
    "World - Paintings": "WorldPaintings",
    "World - Spirit Springs": "WorldSpiritSprings",
    "World - Spiritspring Hawks": "WorldSpiritspringHawks",
    "World - Stakes of Marika": "WorldStakesOfMarika",
    "World - Summoning Pools": "WorldSummoningPools",
    "World - Kindling Spirits": "WorldKindlingSpirits",
    "World - Seal Puzzles": "WorldInteractables",
    "World - Hero's Tomb Statues": "WorldInteractables",
}

# MASSEDIT field name -> C++ struct field name and type
# Types: u16=unsigned short, u32=unsigned int, i32=int, f=float, u8=unsigned char, bit=bitfield
FIELD_MAP = {
    "iconId": ("iconId", "u16"),
    "dispMask00": ("dispMask00", "bit"),
    "dispMask01": ("dispMask01", "bit"),
    "dispMinZoomStep": ("dispMinZoomStep", "u8"),
    "areaNo": ("areaNo", "u8"),
    "gridXNo": ("gridXNo", "u8"),
    "gridZNo": ("gridZNo", "u8"),
    "posX": ("posX", "f"),
    "posY": ("posY", "f"),
    "posZ": ("posZ", "f"),
    "textId1": ("textId1", "i32"),
    "textId2": ("textId2", "i32"),
    "textId3": ("textId3", "i32"),
    "textId4": ("textId4", "i32"),
    "textId5": ("textId5", "i32"),
    "textId6": ("textId6", "i32"),
    "textId7": ("textId7", "i32"),
    "textId8": ("textId8", "i32"),
    "textEnableFlagId1": ("textEnableFlagId1", "u32"),
    "textEnableFlagId2": ("textEnableFlagId2", "u32"),
    "textEnableFlagId4": ("textEnableFlagId4", "u32"),
    "textEnableFlagId5": ("textEnableFlagId5", "u32"),
    "textDisableFlagId1": ("textDisableFlagId1", "u32"),
    "textDisableFlagId2": ("textDisableFlagId2", "u32"),
    "textDisableFlagId3": ("textDisableFlagId3", "u32"),
    "textDisableFlagId4": ("textDisableFlagId4", "u32"),
    "textDisableFlagId5": ("textDisableFlagId5", "u32"),
    "textDisableFlagId6": ("textDisableFlagId6", "u32"),
    "textDisableFlagId7": ("textDisableFlagId7", "u32"),
    "textDisableFlagId8": ("textDisableFlagId8", "u32"),
    "selectMinZoomStep": ("selectMinZoomStep", "u8"),
    "eventFlagId": ("eventFlagId", "u32"),
    "clearedEventFlagId": ("clearedEventFlagId", "u32"),
    "textType2": ("textType2", "u8"),
    "textType3": ("textType3", "u8"),
    # pad2_0 is a 6-bit field at bits 2-7 of byte 0x18
    # value 1 = bit 2 set = dispMask02 in our struct (DLC map layer)
    "pad2_0": ("dispMask02", "bit"),
    # unkC0-unkDC map to textEnableFlag2Id1-8
    "unkC0": ("textEnableFlag2Id1", "i32"),
    "unkC4": ("textEnableFlag2Id2", "i32"),
    "unkC8": ("textEnableFlag2Id3", "i32"),
    "unkCC": ("textEnableFlag2Id4", "i32"),
    "unkD0": ("textEnableFlag2Id5", "i32"),
    "unkD4": ("textEnableFlag2Id6", "i32"),
    "unkD8": ("textEnableFlag2Id7", "i32"),
    "unkDC": ("textEnableFlag2Id8", "i32"),
}

# C++ struct field order (must match WORLD_MAP_POINT_PARAM_ST declaration)
CPP_FIELD_ORDER = [
    "eventFlagId", "distViewEventFlagId", "iconId", "bgmPlaceType",
    "isAreaIcon", "isOverrideDistViewMarkPos", "isEnableNoText",
    "areaNo_forDistViewMark", "gridXNo_forDistViewMark", "gridZNo_forDistViewMark",
    "clearedEventFlagId",
    "dispMask00", "dispMask01", "dispMask02",
    "distViewIconId", "angle",
    "areaNo", "gridXNo", "gridZNo",
    "posX", "posY", "posZ",
    "textId1", "textEnableFlagId1", "textDisableFlagId1",
    "textId2", "textEnableFlagId2", "textDisableFlagId2",
    "textId3", "textEnableFlagId3", "textDisableFlagId3",
    "textId4", "textEnableFlagId4", "textDisableFlagId4",
    "textId5", "textEnableFlagId5", "textDisableFlagId5",
    "textId6", "textEnableFlagId6", "textDisableFlagId6",
    "textId7", "textEnableFlagId7", "textDisableFlagId7",
    "textId8", "textEnableFlagId8", "textDisableFlagId8",
    "textType1", "textType2", "textType3", "textType4",
    "textType5", "textType6", "textType7", "textType8",
    "distViewId", "posX_forDistViewMark", "posY_forDistViewMark", "posZ_forDistViewMark",
    "distViewId1", "distViewId2", "distViewId3",
    "dispMinZoomStep", "selectMinZoomStep", "entryFEType",
    "textEnableFlag2Id1", "textEnableFlag2Id2", "textEnableFlag2Id3", "textEnableFlag2Id4",
    "textEnableFlag2Id5", "textEnableFlag2Id6", "textEnableFlag2Id7", "textEnableFlag2Id8",
    "textDisableFlag2Id1", "textDisableFlag2Id2", "textDisableFlag2Id3", "textDisableFlag2Id4",
    "textDisableFlag2Id5", "textDisableFlag2Id6", "textDisableFlag2Id7", "textDisableFlag2Id8",
]

# Fields to skip
SKIP_FIELDS = {"Name", "pad4"}


# ERR-only MASSEDIT categories: never bake these in the vanilla profile, even
# if a stale file is present in the (gitignored) vanilla output dir.
ERR_ONLY_FILES = {
    "Reforged - Rune Pieces", "Reforged - Ember Pieces",
    "Reforged - Items", "Reforged - Fortunes", "Reforged - Sealed Curios",
    "World - Kindling Spirits",
}


def parse_massedit_files(massedit_dir):
    """Parse all .MASSEDIT files and return dict of {row_id: {field: value, ..., '_category': str}}"""
    import config
    entries = defaultdict(dict)

    for filepath in sorted(Path(massedit_dir).glob("*.MASSEDIT")):
        filename = filepath.stem
        if config.PROFILE != 'err' and filename in ERR_ONLY_FILES:
            print(f"SKIP (ERR-only): {filename}")
            continue
        # WorldBosses are no longer baked: the overlay builds them LIVE from the in-game
        # WorldMapPointParam field-boss rows (textId2==5100) — see map_entry_layer.cpp
        # build_live_bosses() + windows_enemy_boss_runtime_pos_re_findings.md. Reading live is
        # authoritative (kills per-ERR-version drift + the boss_list matching anomalies). The
        # boss_list.json file is kept — generate_loot_massedit / relocating_boss_fix consume it.
        if filename == "World - Bosses":
            print(f"SKIP (drawn live, not baked): {filename}")
            continue
        category = CATEGORY_MAP.get(filename)
        if category is None:
            # Auto-generate category name from filename
            category = re.sub(r'[^A-Za-z0-9]', '', filename.replace(' - ', '_').replace(' ', '_'))
            print(f"INFO: Auto-category for '{filename}' -> {category}")

        # Parse: param WorldMapPointParam: id XXXXX: fieldName: = value;
        pattern = re.compile(
            r"param\s+WorldMapPointParam:\s+id\s+(\d+):\s+(\w+):\s*=\s*(.+);"
        )

        with open(filepath, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                m = pattern.match(line)
                if m:
                    row_id = int(m.group(1))
                    field = m.group(2)
                    value = m.group(3).strip()

                    if field in SKIP_FIELDS:
                        continue

                    if field not in FIELD_MAP:
                        print(f"WARNING: Unknown field '{field}' in {filename}, skipping")
                        continue

                    # Our custom icon frames (sprite-171 349-440) sit at a
                    # different frame index when the target mod's worldmap gfx
                    # already extends the sprite (Convergence: +408). Single
                    # remap chokepoint for every category/MASSEDIT.
                    if field == "iconId" and config.ICON_FRAME_OFFSET:
                        icon = int(float(value))
                        lo, hi = config.OUR_ICON_RANGE
                        if lo <= icon <= hi:
                            value = str(icon + config.ICON_FRAME_OFFSET)

                    entries[row_id][field] = value
                    entries[row_id]["_category"] = category

    # Fix interior areas that don't display correctly on overworld
    # e.g. m11_10 (Roundtable Hold) — MSB coords land in ocean
    # Shift all coords to match grace display positions (area stays unchanged)
    # m11_10: MSB ~(-305,-298) → grace display ~(-2500,-650) = offset (-2195,-352)
    COORD_SHIFTS = {(11, 10): (-2195.0, -352.0)}
    shifted = 0
    for row_id, fields in entries.items():
        area = int(fields.get("areaNo", "0"))
        gx = int(fields.get("gridXNo", "0"))
        shift = COORD_SHIFTS.get((area, gx))
        if shift:
            dx, dz = shift
            fields["posX"] = f"{float(fields.get('posX', '0')) + dx:.3f}"
            fields["posZ"] = f"{float(fields.get('posZ', '0')) + dz:.3f}"
            shifted += 1
    if shifted > 0:
        print(f"  Shifted {shifted} entries (interior coord fix)")

    return entries


def format_value(cpp_field, cpp_type, raw_value):
    if cpp_type == "f":
        v = raw_value.rstrip(";").strip()
        if "." not in v:
            v += ".f"
        else:
            v += "f"
        return v
    elif cpp_type in ("u32", "u16", "u8"):
        return str(int(float(raw_value)))
    elif cpp_type == "i32":
        return str(int(float(raw_value)))
    elif cpp_type == "bit":
        v = int(float(raw_value))
        return "true" if v else "false"
    elif cpp_type == "arr1":
        v = int(float(raw_value))
        return "{" + str(v) + "}"
    return raw_value


def load_piece_metadata(massedit_dir):
    """Load geom_slot and name_suffix from *_slots.json files.
    Returns dict: row_id (int) -> {geom_slot: int, name_suffix: int}."""
    meta = {}
    for path in Path(massedit_dir).glob("*_slots.json"):
        with open(path) as f:
            data = json.load(f)
        for row_id_str, val in data.items():
            if isinstance(val, dict):
                meta[int(row_id_str)] = val
            else:
                # Legacy format: just geom_slot as int
                meta[int(row_id_str)] = {'geom_slot': val, 'name_suffix': -1}
    return meta


def _load_lot_linkage():
    """row_id(int) -> (lotId, lotType, source) from generate_loot_massedit's side file.
    `source` is the provenance string (treasure/enemy/emevd); '' for legacy 2-element
    linkage files written before provenance was added."""
    import config
    p = config.DATA_DIR / 'loot_lot_linkage.json'
    if not p.exists():
        return {}
    with open(p, encoding='utf-8') as f:
        raw = json.load(f)
    return {int(k): (int(v[0]), int(v[1]), (v[2] if len(v) > 2 else ''))
            for k, v in raw.items()}


# Provenance string (extract_all_items 'source') -> MapEntry LootSource enumerator.
# Unknown for anything unmapped (incl. legacy linkage with no source).
_LOOT_SRC_ENUM = {'treasure': 'Treasure', 'enemy': 'Enemy', 'emevd': 'Emevd'}


# Phase-2 no-bake stub written to the COMPILED goblin_map_data.cpp. The static marker bake is
# retired — every marker comes from live mod files / game memory at runtime — so the compiled
# table is empty. A 0-length array is ill-formed, so a 1-elem zero dummy is emitted and never
# read (every consumer iterates `i < MAP_ENTRY_COUNT` == 0). The Category enum + MapEntry types
# stay in goblin_map_data.hpp (they drive the overlay layers/buckets).
_MAP_DATA_STUB = (
    "// AUTO-GENERATED FILE - DO NOT EDIT\n"
    "// Generated by tools/generate_data.py — PHASE-2 NO-BAKE STUB (the static marker bake is\n"
    "// retired; every marker comes from live/disk at runtime). The full MapEntry table lives in\n"
    "// the uncompiled intermediate data/_map_entries_full.cpp (consumed only by generate_geof_models\n"
    "// + generate_location_overrides). See the Phase-2 memo for the path to dropping those too.\n\n"
    '#include "goblin_map_data.hpp"\n\n'
    "namespace goblin::generated\n{\n\n"
    "const size_t MAP_ENTRY_COUNT = 0;\n\n"
    "// Never read (COUNT == 0 gates every consumer loop); a 0-length array is ill-formed.\n"
    "const MapEntry MAP_ENTRIES[1] = {};\n\n"
    "} // namespace goblin::generated\n"
)


def generate_map_data_cpp(entries, output_path, geom_slots=None, intermediate_path=None):
    """Phase 2 (no-bake): the COMPILED goblin_map_data.cpp is an EMPTY STUB. The FULL MapEntry
    table is still emitted, but to a pipeline INTERMEDIATE (intermediate_path, uncompiled) because
    two downstream generators text-parse it: generate_geof_models (row_id -> actual model) and
    generate_location_overrides (row_id -> position). When intermediate_path is None this falls back
    to the legacy behaviour (full table -> output_path) for any old caller."""
    if geom_slots is None:
        geom_slots = {}

    lot_linkage = _load_lot_linkage()

    # Sort entries by row_id
    sorted_ids = sorted(entries.keys())

    # Full table -> the intermediate (uncompiled) when given; else legacy (-> output_path).
    full_target = intermediate_path if intermediate_path is not None else output_path
    with open(full_target, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED INTERMEDIATE - DO NOT EDIT, DO NOT COMPILE\n")
        f.write("// Full MapEntry table (Phase-2 no-bake). The COMPILED goblin_map_data.cpp is an\n")
        f.write("// empty stub; this uncompiled copy feeds generate_geof_models +\n")
        f.write("// generate_location_overrides only (they text-parse the '// Row ID' markers).\n\n")
        f.write('#include "goblin_map_data.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")

        f.write(f"const size_t MAP_ENTRY_COUNT = {len(sorted_ids)};\n\n")
        f.write("const MapEntry MAP_ENTRIES[] = {\n")

        for row_id in sorted_ids:
            fields = entries[row_id]
            category = fields.get("_category", "World")

            f.write(f"    // Row ID {row_id}\n")
            f.write(f"    {{{row_id}ull, {{\n")

            field_dict = {}
            for massedit_field, raw_value in fields.items():
                if massedit_field.startswith("_"):
                    continue
                if massedit_field in SKIP_FIELDS:
                    continue
                cpp_field, cpp_type = FIELD_MAP[massedit_field]
                formatted = format_value(cpp_field, cpp_type, raw_value)
                field_dict[cpp_field] = formatted

            meta = geom_slots.get(row_id, {})
            slot = meta.get('geom_slot', -1) if isinstance(meta, dict) else meta
            suffix = meta.get('name_suffix', -1) if isinstance(meta, dict) else -1
            obj_name = meta.get('object_name', '') if isinstance(meta, dict) else ''
            lot_id, lot_type, lot_src = lot_linkage.get(row_id, (0, 0, ''))
            lot_backed = lot_id and lot_type
            src_enum = _LOOT_SRC_ENUM.get(lot_src, 'Unknown')

            field_assignments = []
            for cpp_field in CPP_FIELD_ORDER:
                if cpp_field in field_dict:
                    # Lot-backed loot IDENTITY is read LIVE from ItemLotParam at runtime
                    # (map_entry_layer resolve_loot_item_textid) — don't bake textId1; the
                    # struct default (-1) is never read for these rows. Keeps the bake free of
                    # the redundant item key (live == baked verified). See loot docs.
                    if cpp_field == "textId1" and lot_backed:
                        continue
                    field_assignments.append((cpp_field, field_dict[cpp_field]))

            for cpp_field, formatted in field_assignments:
                f.write(f"        .{cpp_field} = {formatted},\n")

            name_field = f'"{obj_name}"' if obj_name else 'nullptr'
            f.write(f"    }}, Category::{category}, {slot}, {suffix}, {name_field}, "
                    f"{lot_id}u, {lot_type}, LootSource::{src_enum}}},\n")

        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {full_target} with {len(sorted_ids)} entries")

    # Retire the bake from the DLL: the COMPILED file is the empty stub. Skipped when
    # intermediate_path is None (legacy callers that want the full table in output_path).
    if intermediate_path is not None:
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(_MAP_DATA_STUB)
        print(f"Generated {output_path} (Phase-2 empty stub; {len(sorted_ids)} rows in the intermediate)")


def generate_item_icons_cpp(output_path):
    """Generate goblin_item_icons.cpp: encoded-item-key -> (iconId, Category).

    Source = item_icon_table.json from generate_loot_massedit (the same ordered
    LOOT_CATEGORIES classifier, applied per item). Lets the DLL re-icon and
    re-gate a lot-backed marker from the LIVE randomized item. Sorted by key
    for binary search."""
    import config
    p = config.DATA_DIR / 'item_icon_table.json'
    table = {}  # key(int) -> iconId  (Phase-3: no Category column — see goblin_category_exceptions)
    if p.exists():
        with open(p, encoding='utf-8') as f:
            raw = json.load(f)
        for k, v in raw.items():
            table[int(k)] = int(v[0])

    off = config.ICON_FRAME_OFFSET
    # Spoiler-free "?" map-icon frame. Our 92 custom icons occupy iconIds
    # OUR_ICON_RANGE = 349..440 (the game's iconId is the 1-based sprite-171 frame
    # number; our frames append right after the base's frames). The "?" frame is
    # appended AFTER our 92 icons, so it is iconId 441 (= 440 + 1), NOT 440 — 440
    # is our LAST real icon and pointing the anon override there showed that icon
    # (a statue on Convergence) instead of the "?". On overlay bases that add
    # their own frames (Convergence) every iconId shifts by ICON_FRAME_OFFSET, so
    # the "?" is 441 + offset (441 vanilla/erte/err, 849 convergence). build_vanilla_gfx
    # places the frame at 0-indexed position 440(+offset); that 0-indexed position
    # is 1-based iconId 441(+offset) in-game — the +1 is this off-by-one.
    anon_icon_id = 441 + off
    # Appended frames after the "?" (build_vanilla_gfx), each inheriting the same
    # per-profile offset: cluster = anon+1 (442), quest-NPC = anon+2 (443, no DLL
    # const — its iconId is baked into the MASSEDIT), cluster-depleted = anon+3 (444).
    cluster_icon_id = anon_icon_id + 1
    cluster_done_icon_id = anon_icon_id + 3

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from item_icon_table.json\n\n")
        f.write('#include "goblin_item_icons.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const uint16_t ANON_ICON_ID = {anon_icon_id}u;\n")
        f.write(f"const uint16_t CLUSTER_ICON_ID = {cluster_icon_id}u;\n")
        f.write(f"const uint16_t CLUSTER_DONE_ICON_ID = {cluster_done_icon_id}u;\n\n")
        f.write(f"const size_t ITEM_ICON_COUNT = {len(table)};\n\n")
        f.write("const ItemIcon ITEM_ICONS[] = {\n")
        for key in sorted(table.keys()):
            icon = table[key]
            f.write(f"    {{{key}, {icon + off}u}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(table)} item-icon entries")


# Phase-3: item categories the mod assigns by a deliberate id-list/name rule that ER's own
# taxonomy (goodsType, sortGroupId) cannot express — they STAY as a curated per-item table.
# Everything else is classified live from (goodsType, sortGroupId) in the DLL (drift-free).
# Validated to reproduce the old ITEM_ICONS category column exactly (_validate_taxonomy_map.py).
CATEGORY_EXCEPTION_CATS = {
    "Loot - Golden Runes (Low)",
    "Loot - Smithing Stones (Low)", "Loot - Smithing Stones (Rare)",
    "Loot - Great Gloveworts",
    "Loot - Rune Arcs", "Loot - Prattling Pates", "Loot - MP-Fingers", "Loot - Rada Fruit",
    "Key - Celestial Dew", "Key - Imbued Sword Keys", "Loot - Stonesword Keys",
    "Quest - Deathroot", "Quest - Seedbed Curses",
    "Key - Whetblades", "Key - Larval Tears", "Key - Lost Ashes", "Loot - Dragon Hearts",
    "Key - Scadutree Fragments", "Key - Seeds Tears Ashes",
    "Magic - Memory Stones", "Magic - Prayerbooks",
    "Quest - Progression",
    "Reforged - Items", "Reforged - Fortunes", "Reforged - Sealed Curios",
}


def generate_category_exceptions_cpp(output_path):
    """Generate goblin_category_exceptions.cpp: raw goods id -> Category, for the
    curated splits/grab-bags ER's own taxonomy can't express (Phase-3). Source =
    item_icon_table.json (the LOOT_CATEGORIES classifier); only goods whose category
    is in CATEGORY_EXCEPTION_CATS are emitted. Sorted by id for binary search."""
    import config
    p = config.DATA_DIR / 'item_icon_table.json'
    rows = {}  # goods_id(int) -> Category enum name
    if p.exists():
        with open(p, encoding='utf-8') as f:
            raw = json.load(f)
        for k, v in raw.items():
            key = int(k)
            if key < 500000000:
                continue  # exceptions are goods-only (non-goods classify by key range)
            cat_name = v[1]
            if cat_name not in CATEGORY_EXCEPTION_CATS:
                continue
            enum = CATEGORY_MAP.get(cat_name)
            if enum:
                rows[key - 500000000] = enum

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from item_icon_table.json\n\n")
        f.write('#include "goblin_category_exceptions.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const size_t CATEGORY_EXCEPTION_COUNT = {len(rows)};\n\n")
        f.write("const CategoryException CATEGORY_EXCEPTIONS[] = {\n")
        for gid in sorted(rows.keys()):
            f.write(f"    {{{gid}, Category::{rows[gid]}}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")

    print(f"Generated {output_path} with {len(rows)} category exceptions")


# Fixed language order for the embedded enemy-name table (msgbnd codes). engus
# first so it can serve as the fallback. Must match ENEMY_NAME_LANGS in the DLL.
ENEMY_NAME_LANGS = ["engus", "jpnjp", "deude", "frafr", "itait", "korkr", "polpl",
                    "porbr", "rusru", "spaes", "spaar", "thath", "zhocn", "zhotw", "araae"]


def _wlit(s):
    """C++ wide-string literal with every non-ASCII unit as \\uXXXX (encoding-safe)."""
    units = s.encode("utf-16-le")
    out = []
    for i in range(0, len(units), 2):
        cu = units[i] | (units[i + 1] << 8)
        if 0x20 <= cu < 0x7f and chr(cu) not in '"\\':
            out.append(chr(cu))
        else:
            out.append("\\u%04x" % cu)
    return 'L"' + "".join(out) + '"'


def generate_enemy_names_cpp(output_path):
    """Generate goblin_enemy_names.cpp: localized enemy names for the non-ERR
    builds (marker textId = id + 900000000). Empty for the ERR build (it uses
    its own runtime name table). Strings are FromSoft / community-wiki enemy
    names; no reference to where the source data was read from."""
    import config
    is_err = (config.PROFILE == "err")
    table = {}
    if not is_err:
        p = config.PROJECT_DIR / "data" / "enemy_names_i18n.json"
        if p.exists():
            with open(p, encoding="utf-8") as f:
                table = json.load(f)

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from enemy_names_i18n.json\n\n")
        f.write('#include "goblin_enemy_names.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write("const char *const ENEMY_NAME_LANGS[ENEMY_NAME_LANG_COUNT] = {\n    ")
        f.write(", ".join(f'"{c}"' for c in ENEMY_NAME_LANGS))
        f.write("\n};\n\n")
        f.write(f"const size_t ENEMY_NAME_COUNT = {len(table)};\n\n")
        f.write("const EnemyName ENEMY_NAMES[] = {\n")
        for k in sorted(table, key=lambda x: int(x)):
            langs = table[k]
            en = langs.get("engus", "")
            cells = [_wlit(langs.get(code, en)) for code in ENEMY_NAME_LANGS]
            f.write(f"    {{{int(k)}, {{{', '.join(cells)}}}}},\n")
        if not table:
            f.write("    {0, {" + ", ".join(["nullptr"] * len(ENEMY_NAME_LANGS)) + "}},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"Generated {output_path} with {len(table)} enemy-name entries"
          + (" (ERR: empty, uses runtime table)" if is_err else ""))


# Offset-encoded name-id scheme, identical to encode_live_item() in
# goblin_inject.cpp and the markers' baked textId1 (so the alias keys line up
# with Marker::name_id at runtime). lotItemCategory -> id offset.
ITEM_NAME_CAT_OFFSET = {1: 500000000, 2: 100000000, 3: 200000000,
                        4: 300000000, 5: 400000000}


def _load_json(path):
    return json.load(open(path, encoding="utf-8")) if path.exists() else None


def generate_name_aliases_en_cpp(output_path):
    """Generate goblin_name_aliases_en.cpp: an English name-alias table for the F1
    marker search, keyed by the encoded name id the markers carry. Aggregates every
    committed English-name source (always extracted from the engus FMGs), so search
    matches the English/wiki name regardless of the player's game language:
      items   from items_database.json (encode_live_item offsets)
      NPCs    from npc_name_text_map.json (id + 700M)
      enemies from enemy_names_i18n.json engus column (id + 900M)
      bosses  from boss_list.json wmpTextId1 -> vanillaPlaceName (raw)."""
    import config
    data = config.PROJECT_DIR / "data"
    table = {}  # encoded_id -> English name (first-wins; sources ordered by priority)

    def add(enc, nm):
        # Marker::name_id is int32; an out-of-range encoded id can never match a
        # marker (e.g. the INT32_MAX "ERR dummy" NpcName sentinel) — drop it.
        if enc and -2**31 <= enc < 2**31 and nm and "<" not in nm:
            table.setdefault(enc, nm)

    # Items (offset-encoded by lotItemCategory).
    db = _load_json(data / "items_database.json")
    for lot in (db or []):
        for it in (lot.get("items") or []):
            off = ITEM_NAME_CAT_OFFSET.get(it.get("category"))
            if off is not None and it.get("id") is not None:
                add(it["id"] + off, it.get("name"))

    # Named NPCs (hostile invaders / quest NPCs): marker textId = nameId + 700M.
    npc = _load_json(data / "npc_name_text_map.json")
    for k, v in (npc or {}).items():
        add(int(k) + 700000000, v)

    # Enemy / Codex names: marker textId = id + 900M (engus column).
    enemies = _load_json(data / "enemy_names_i18n.json")
    for k, langs in (enemies or {}).items():
        add(int(k) + 900000000, (langs or {}).get("engus"))

    # Bosses: the baked marker carries the raw WorldMapPlaceName textId1.
    for b in (_load_json(data / "boss_list.json") or []):
        add(b.get("wmpTextId1"), b.get("vanillaPlaceName"))

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED FILE - DO NOT EDIT\n")
        f.write("// Generated by tools/generate_data.py from items_database.json,\n")
        f.write("// npc_name_text_map.json, enemy_names_i18n.json, boss_list.json\n\n")
        f.write('#include "goblin_name_aliases_en.hpp"\n\n')
        f.write("namespace goblin::generated\n{\n\n")
        f.write(f"const size_t NAME_ALIAS_EN_COUNT = {len(table)};\n\n")
        f.write("const NameAliasEn NAME_ALIASES_EN[] = {\n")
        for enc in sorted(table):
            f.write(f"    {{{enc}, {_wlit(table[enc])}}},\n")
        if not table:
            f.write("    {0, nullptr},\n")
        f.write("};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"Generated {output_path} with {len(table)} English name aliases")


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--massedit-dir", type=str, default=None,
                        help="Path to MASSEDIT directory (default: data/massedit)")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_dir = script_dir.parent

    if args.massedit_dir:
        massedit_dir = Path(args.massedit_dir)
    else:
        massedit_dir = project_dir / "data" / "massedit"
    import config
    output_dir = config.GENERATED_DIR  # src/generated or src/generated_vanilla

    output_dir.mkdir(parents=True, exist_ok=True)

    # goblin_map_data.hpp (the Category enum + struct) is a static, profile-
    # independent header with no generator. The canonical copy lives in
    # src/generated/; mirror it into a non-default bake dir (e.g. vanilla) so
    # that dir is self-contained for the compiler's include path.
    import shutil
    for hdr in ("goblin_map_data.hpp", "goblin_item_icons.hpp",
                "goblin_category_exceptions.hpp", "goblin_enemy_names.hpp",
                "goblin_name_aliases_en.hpp"):
        canonical_hpp = project_dir / "src" / "generated" / hdr
        dest_hpp = output_dir / hdr
        if canonical_hpp.resolve() != dest_hpp.resolve():
            shutil.copyfile(canonical_hpp, dest_hpp)

    # goblin_tile_tabs.{hpp,cpp} (player MapId tile -> map sub-page tabId, from
    # build_tile_tabs.py) is profile-independent — the game's tiling is identical
    # across bakes — so mirror the committed canonical pair into a non-default dir.
    for f in ("goblin_tile_tabs.hpp", "goblin_tile_tabs.cpp",
              "goblin_major_regions.hpp", "goblin_major_regions.cpp"):
        canonical = project_dir / "src" / "generated" / f
        dest = output_dir / f
        if canonical.exists() and canonical.resolve() != dest.resolve():
            shutil.copyfile(canonical, dest)

    print("=== Parsing MASSEDIT files ===")
    entries = parse_massedit_files(massedit_dir)
    print(f"Total unique entries: {len(entries)}")

    # De-overlap: shift icons that share exact coordinates using a square spiral
    print("\n=== De-overlapping icons ===")
    SPIRAL_STEP = 8.0  # world units per spiral step
    coord_groups = {}  # (areaNo, gridX, gridZ, posX_round, posZ_round) -> [row_ids]
    for row_id, fields in entries.items():
        area = fields.get("areaNo", "0")
        gx = fields.get("gridXNo", "0")
        gz = fields.get("gridZNo", "0")
        px = round(float(fields.get("posX", "0")), 1)
        pz = round(float(fields.get("posZ", "0")), 1)
        key = (area, gx, gz, px, pz)
        coord_groups.setdefault(key, []).append(row_id)

    def spiral_offsets(n):
        """Generate n (dx, dz) offsets in a square spiral pattern.
        (0,0), (0,-1), (1,-1), (1,0), (1,1), (0,1), (-1,1), (-1,0), (-1,-1), (0,-2), ...
        """
        offsets = []
        x = z = 0
        d = 1  # current ring distance
        offsets.append((0, 0))
        while len(offsets) < n:
            # Up: (0,-d)
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x, -i + z))
            z -= d
            # Right+down diagonal to (d, 0) relative
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x + i, z + i))
            x += d; z += d
            # Down
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x, z + i))
            z += d
            # Left
            for i in range(1, 2 * d + 1):
                if len(offsets) >= n: break
                offsets.append((x - i, z))
            x -= 2 * d
            # Up
            for i in range(1, 2 * d + 1):
                if len(offsets) >= n: break
                offsets.append((x, z - i))
            z -= 2 * d
            # Right (partial, to start next ring)
            for i in range(1, d + 1):
                if len(offsets) >= n: break
                offsets.append((x + i, z))
            x += d
            d += 1
        return offsets[:n]

    shifted = 0
    for key, row_ids in coord_groups.items():
        if len(row_ids) <= 1:
            continue
        offsets = spiral_offsets(len(row_ids))
        for i, row_id in enumerate(row_ids):
            if i == 0:
                continue  # first stays in place
            ox, oz = offsets[i]
            old_px = float(entries[row_id].get("posX", "0"))
            old_pz = float(entries[row_id].get("posZ", "0"))
            entries[row_id]["posX"] = f"{old_px + ox * SPIRAL_STEP:.3f}"
            entries[row_id]["posZ"] = f"{old_pz + oz * SPIRAL_STEP:.3f}"
            shifted += 1

    print(f"  Shifted {shifted} entries across {sum(1 for v in coord_groups.values() if len(v) > 1)} groups")

    print("\n=== Loading piece metadata ===")
    geom_slots = load_piece_metadata(massedit_dir)
    print(f"Loaded {len(geom_slots)} piece metadata entries")

    print("\n=== Generating map data C++ (stub) + full intermediate ===")
    import config
    generate_map_data_cpp(entries, output_dir / "goblin_map_data.cpp", geom_slots,
                          intermediate_path=config.DATA_DIR / "_map_entries_full.cpp")

    print("\n=== Generating item-icon table C++ ===")
    generate_item_icons_cpp(output_dir / "goblin_item_icons.cpp")

    print("\n=== Generating category-exceptions table C++ ===")
    generate_category_exceptions_cpp(output_dir / "goblin_category_exceptions.cpp")

    print("\n=== Generating enemy-name table C++ ===")
    generate_enemy_names_cpp(output_dir / "goblin_enemy_names.cpp")

    print("\n=== Generating English name-alias table C++ ===")
    generate_name_aliases_en_cpp(output_dir / "goblin_name_aliases_en.cpp")

    print("\n=== Generating legacy-conv C++ ===")
    generate_legacy_conv_cpp(config.DATA_DIR / "WorldMapLegacyConvParam.json",
                             output_dir / "goblin_legacy_conv.hpp")

    print("\nDone.")


def generate_legacy_conv_cpp(conv_json, output_path):
    """Emit a C++ header with the dungeon→overworld conversion table.
    Used by goblin_markers to place dungeon MAP_ENTRIES on overworld coords."""
    if not conv_json.exists():
        print(f"  WARNING: {conv_json} missing, skipping")
        return
    rows = json.load(open(conv_json, encoding='utf-8'))
    # Parse every hop. The param maps a sub-area onto another area; many dungeon
    # sub-areas reach the overworld (60/61) only TRANSITIVELY — e.g. Leyndell's
    # Ashen Capital m35 -> area 11 (Leyndell) -> area 60. A single-hop filter
    # (dst in 60/61) silently drops those, so follow the chains and compose the
    # translations down to 60/61.
    hops = []
    for r in rows:
        if not isinstance(r, dict): continue
        hops.append({
            'src_area': int(r.get('srcAreaNo', 0)),
            'src_gx': int(r.get('srcGridXNo', 0)),
            'src_gz': int(r.get('srcGridZNo', 0)),
            'src_pos_x': float(r.get('srcPosX', 0)),
            'src_pos_z': float(r.get('srcPosZ', 0)),
            'dst_area': int(r.get('dstAreaNo', 0)),
            'dst_gx': int(r.get('dstGridXNo', 0)),
            'dst_gz': int(r.get('dstGridZNo', 0)),
            'dst_pos_x': float(r.get('dstPosX', 0)),
            'dst_pos_z': float(r.get('dstPosZ', 0)),
        })
    by_src = {}   # (area, gx) -> first hop
    by_area = {}  # area -> first hop (grid fallback)
    for h in hops:
        by_src.setdefault((h['src_area'], h['src_gx']), h)
        by_area.setdefault(h['src_area'], h)

    def w(gx, pos): return gx * 256.0 + pos

    def resolve(h, seen):
        """Compose chains until dst is 60/61; None if it never reaches overworld."""
        if h['dst_area'] in (60, 61):
            return h
        if h['dst_area'] in seen:  # cycle guard
            return None
        nxt = by_src.get((h['dst_area'], h['dst_gx'])) or by_area.get(h['dst_area'])
        if not nxt:
            return None
        r = resolve(nxt, seen | {h['dst_area']})
        if not r:
            return None
        # Translate H's dst world point by R's (dst - src) offset.
        cx = w(h['dst_gx'], h['dst_pos_x']) + w(r['dst_gx'], r['dst_pos_x']) - w(r['src_gx'], r['src_pos_x'])
        cz = w(h['dst_gz'], h['dst_pos_z']) + w(r['dst_gz'], r['dst_pos_z']) - w(r['src_gz'], r['src_pos_z'])
        gx, gz = int(cx // 256), int(cz // 256)
        return {**h, 'dst_area': r['dst_area'], 'dst_gx': gx, 'dst_gz': gz,
                'dst_pos_x': cx - gx * 256.0, 'dst_pos_z': cz - gz * 256.0}

    by_key = {}
    for h in hops:
        key = (h['src_area'], h['src_gx'])
        if key in by_key:
            continue
        res = resolve(h, {h['src_area']})
        if res:
            by_key[key] = res
    entries = sorted(by_key.values(), key=lambda e: (e['src_area'], e['src_gx']))
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("#pragma once\n// AUTO-GENERATED — do not edit.\n")
        f.write("// Dungeon-area → overworld-tile conversion table (first base-point per src key,\n")
        f.write("// transitively composed down to area 60/61 — e.g. Ashen Capital m35→11→60).\n\n")
        f.write("#include <cstdint>\n#include <cstddef>\n\n")
        f.write("namespace goblin::generated {\n\n")
        f.write("struct LegacyConvEntry {\n")
        f.write("    uint8_t src_area;\n    uint8_t src_gx;\n    uint8_t src_gz;\n")
        f.write("    float src_pos_x;\n    float src_pos_z;\n")
        f.write("    uint8_t dst_area;\n    uint8_t dst_gx;\n    uint8_t dst_gz;\n")
        f.write("    float dst_pos_x;\n    float dst_pos_z;\n")
        f.write("};\n\n")
        f.write(f"constexpr LegacyConvEntry LEGACY_CONV[] = {{\n")
        for e in entries:
            f.write(f"    {{ {e['src_area']}, {e['src_gx']}, {e['src_gz']}, {e['src_pos_x']:.3f}f, {e['src_pos_z']:.3f}f, ")
            f.write(f"{e['dst_area']}, {e['dst_gx']}, {e['dst_gz']}, ")
            f.write(f"{e['dst_pos_x']:.3f}f, {e['dst_pos_z']:.3f}f }},\n")
        f.write("};\n\n")
        f.write(f"constexpr size_t LEGACY_CONV_COUNT = {len(entries)};\n\n")
        f.write("} // namespace goblin::generated\n")
    print(f"  Generated {output_path.name} with {len(entries)} conv entries")


if __name__ == "__main__":
    main()
