#!/usr/bin/env python3
"""Coverage vs MapGenie — which categories MapForGoblins still doesn't show.

Compares the RUNTIME [COVERAGE] drawn counts (baked+disk+live, parsed from the log — the
authoritative post-no-bake source; the old goblin_map_data parse was STALE once categories moved
off the bake) against MapGenie's per-category counts. The point is the gap report:
  ❌ NOT WIRED  — a MapGenie type the mod has no category for at all (whole categories still
                  unparsed: Divine Tower, Dungeon, Landmark, Martyr Effigy, Remembrance, Scarabs,
                  friendly NPCs, Elite/Enemy …).
  ⚪ WIRED-EMPTY — a mod category exists but drew 0 this build (not placed / disabled).
  ⚠️ DRIFT       — wired and drawn but fewer than MapGenie (a real coverage gap).
  ℹ️ mod>MG      — the mod shows more (ERR adds content / different taxonomy) — informational.

MapGenie is per-area (60 base, 61 DLC); the runtime [COVERAGE] is GLOBAL (all areas summed), so we
compare against the base+DLC sum. (Per-area would need per-area runtime counts — a future runtime
change.) Graces are drawn by the always-on GraceLayer, NOT in the g_buckets scoreboard, so they're
reported separately as a known special case. Writes docs/coverage_vs_mapgenie.md.

Usage: tools/coverage_vs_mapgenie.py [log]
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
DEFAULT_LOG = Path(r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6\dll\offline\logs\MapForGoblins.log")

# ── MapGenie counts per area, flat by label (across all sections) ─────────────
MAPGENIE = {
    60: {  # The Lands Between
        "Site of Grace": 309, "Stake of Marika": 97, "Spiritspring Jump": 38,
        "Imp Seal Statue": 38, "Divine Tower": 6, "Dragon Shrine": 2,
        "Dungeon": 53, "Elevator": 24, "Evergaol": 11, "Hidden Passage": 40,
        "Landmark": 131, "Legacy Dungeon": 7, "Martyr Effigy": 156,
        "Minor Erdtree": 11, "Portal": 36, "Smithing Table": 1,
        "Wandering Mausoleum": 7,
        "Bell Bearing": 52, "Cookbook": 59, "Cracked Pot": 14, "Great Rune": 10,
        "Key Item": 68, "Map Fragment": 19, "Memory Stone": 8, "Painting": 7,
        "Perfume Bottle": 10, "Remembrance": 15, "Ritual Pot": 9, "Spellbook": 11,
        "Stonesword Key": 65, "Talisman Pouch": 3, "Tool": 23, "Whetblade": 6,
        "Ammunition": 98, "Cerulean Scarab": 21, "Consumable": 265,
        "Crimson Scarab": 40, "Crystal Tear": 32, "Deathroot": 9,
        "Dragon Heart": 13, "Gesture": 35, "Golden Rune": 407, "Golden Seed": 42,
        "Item": 9, "Larval Tear": 17, "Multiplayer Item": 34, "Rune Arc": 63,
        "Sacred Tear": 12, "Spirit Ashes": 65,
        "Armor": 164, "Ash of War": 105, "Incantation": 101, "Shield": 69,
        "Sorcery": 73, "Talisman": 122, "Weapon": 315,
        "Character": 109, "Ghost": 36, "Merchant": 40, "Trainer": 1,
        "Boss": 140, "Elite Enemy": 144, "Enemy": 58, "Great Boss": 14,
        "Invasion": 36, "Legendary Boss": 16,
        "Ancient Smithing Stone": 21, "Crafting Material": 622,
        "Ghost Glovewort": 10, "Glovewort": 119, "Great Glovewort": 9,
        "Miquella's Lily": 69, "Ruin Fragment": 32, "Smithing Stone": 425,
        "Trina's Lily": 148,
    },
    61: {  # The Shadow Realm (DLC)
        "Site of Grace": 105, "Stake of Marika": 22, "Spiritspring Jump": 18,
        "Dungeon": 11, "Elevator": 16, "Hidden Passage": 19, "Landmark": 41,
        "Martyr Effigy": 56, "Miquella's Cross": 13, "Portal": 3,
        "Bell Bearing": 10, "Cookbook": 45, "Cracked Pot": 10, "Great Rune": 1,
        "Key Item": 17, "Map Fragment": 5, "Painting": 3, "Remembrance": 10,
        "Revered Spirit Ash": 23, "Scadutree Fragment": 42, "Tool": 7,
        "Ammunition": 22, "Consumable": 96, "Crystal Tear": 8, "Dragon Heart": 6,
        "Gesture": 4, "Golden Rune": 55, "Item": 4, "Larval Tear": 9,
        "Multiplayer Item": 3, "Rune Arc": 5, "Spirit Ashes": 20,
        "Armor": 46, "Ash of War": 23, "Incantation": 28, "Shield": 11,
        "Sorcery": 14, "Talisman": 40, "Weapon": 92,
        "Character": 18, "Ghost": 21, "Merchant": 3,
        "Boss": 33, "Elite Enemy": 40, "Enemy": 24, "Great Boss": 1,
        "Invasion": 13, "Legendary Boss": 10,
        "Ancient Smithing Stone": 13, "Crafting Material": 507,
        "Ghost Glovewort": 3, "Glovewort": 28, "Great Glovewort": 9,
        "Smithing Stone": 97, "Somber Smithing Stone": 45,
        "Lore": 6, "Miscellaneous": 9, "Puzzle": 8, "Quest (steps)": 7,
        "Stone Cairn": 5, "Summoning Sigil": 6,
    },
}

# enum (Category) → category_name() display string (src/goblin_markers.cpp). The [COVERAGE] log
# keys by this display string, so SECTIONS (enum names) map through here to the parsed drawn counts.
ENUM2DISPLAY = {
    "EquipArmaments": "Equipment - Armaments", "EquipArmour": "Equipment - Armour",
    "EquipAshesOfWar": "Equipment - Ashes of War", "EquipSpirits": "Equipment - Spirits",
    "EquipTalismans": "Equipment - Talismans", "KeyCelestialDew": "Key - Celestial Dew",
    "KeyCookbooks": "Key - Cookbooks", "KeyCrystalTears": "Key - Crystal Tears",
    "KeyImbuedSwordKeys": "Key - Imbued Sword Keys", "KeyLarvalTears": "Key - Larval Tears",
    "KeyScadutreeFragments": "Key - Scadutree Fragments", "KeyGreatRunes": "Key - Great Runes",
    "KeyLostAshes": "Key - Lost Ashes", "KeyPotsNPerfumes": "Key - Pots n Perfumes",
    "KeySeedsTears": "Key - Seeds Tears Ashes", "KeyWhetblades": "Key - Whetblades",
    "LootAmmo": "Loot - Ammo", "LootBellBearings": "Loot - Bell-Bearings",
    "LootMerchantBellBearings": "Loot - Merchant Bell-Bearings", "LootConsumables": "Loot - Consumables",
    "LootCraftingMaterials": "Loot - Crafting Materials", "LootMPFingers": "Loot - MP-Fingers",
    "LootMaterialNodes": "Loot - Material Nodes", "LootReusables": "Loot - Reusables",
    "LootSmithingStones": "Loot - Smithing Stones", "LootSmithingStonesLow": "Loot - Smithing Stones (Low)",
    "LootSmithingStonesRare": "Loot - Smithing Stones (Rare)", "LootGoldenRunes": "Loot - Golden Runes",
    "LootGoldenRunesLow": "Loot - Golden Runes (Low)", "LootStoneswordKeys": "Loot - Stonesword Keys",
    "LootThrowables": "Loot - Throwables", "LootPrattlingPates": "Loot - Prattling Pates",
    "LootRuneArcs": "Loot - Rune Arcs", "LootDragonHearts": "Loot - Dragon Hearts",
    "LootGloveworts": "Loot - Gloveworts", "LootGreatGloveworts": "Loot - Great Gloveworts",
    "LootRadaFruit": "Loot - Rada Fruit", "LootGestures": "Loot - Gestures",
    "LootGreases": "Loot - Greases", "LootUtilities": "Loot - Utilities",
    "LootStatBoosts": "Loot - Stat Boosts", "ReforgedFortunes": "Reforged - Fortunes",
    "MagicIncantations": "Magic - Incantations", "MagicMemoryStones": "Magic - Memory Stones",
    "MagicPrayerbooks": "Magic - Prayerbooks", "MagicSorceries": "Magic - Sorceries",
    "WorldBosses": "World - Bosses", "QuestDeathroot": "Quest - Deathroot",
    "QuestProgression": "Quest - Progression", "QuestSeedbedCurses": "Quest - Seedbed Curses",
    "ReforgedEmberPieces": "Reforged - Ember Pieces", "ReforgedItemsAndChanges": "Reforged - Items",
    "ReforgedRunePieces": "Reforged - Rune Pieces", "WorldGraces": "World - Graces",
    "WorldHostileNPC": "World - Hostile NPC", "WorldQuestNPC": "World - Quest NPC",
    "WorldImpStatues": "World - Imp Statues", "WorldMaps": "World - Maps",
    "WorldPaintings": "World - Paintings", "WorldSpiritSprings": "World - Spirit Springs",
    "WorldSpiritspringHawks": "World - Spiritspring Hawks", "WorldStakesOfMarika": "World - Stakes of Marika",
    "WorldSummoningPools": "World - Summoning Pools", "WorldKindlingSpirits": "World - Kindling Spirits",
    "WorldInteractables": "World - Interactables",
    "WorldDivineTower": "World - Divine Towers", "WorldEvergaol": "World - Evergaols",
    "WorldMinorErdtree": "World - Minor Erdtrees", "WorldGrandLift": "World - Grand Lifts",
    "WorldDungeon": "World - Dungeons", "WorldLegacyDungeon": "World - Legacy Dungeons",
    "WorldMiquellaCross": "World - Miquella's Cross",
}

# Sections: (display, [MapGenie labels to sum], [mod Category enums to sum]).
# Empty mod list = MapGenie type with no mod equivalent (❌ NOT WIRED).
SECTIONS = {
    "Locations": [
        ("Site of Grace", ["Site of Grace"], ["WorldGraces"]),  # graces = live GraceLayer, see note
        ("Stake of Marika", ["Stake of Marika"], ["WorldStakesOfMarika"]),
        ("Spiritspring Jump", ["Spiritspring Jump"], ["WorldSpiritSprings", "WorldSpiritspringHawks"]),
        ("Imp Seal Statue", ["Imp Seal Statue"], ["WorldImpStatues"]),
        ("Divine Tower", ["Divine Tower"], ["WorldDivineTower"]),
        ("Dragon Shrine", ["Dragon Shrine"], []),
        ("Dungeon", ["Dungeon"], ["WorldDungeon"]),
        ("Elevator", ["Elevator"], ["WorldGrandLift"]),  # WMPP has only the 2 grand lifts; MapGenie's 40 in-dungeon lifts are not WMPP
        ("Evergaol", ["Evergaol"], ["WorldEvergaol"]),
        ("Hidden Passage", ["Hidden Passage"], []),
        ("Landmark", ["Landmark"], []),
        ("Legacy Dungeon", ["Legacy Dungeon"], ["WorldLegacyDungeon"]),
        ("Martyr Effigy", ["Martyr Effigy"], []),
        ("Minor Erdtree", ["Minor Erdtree"], ["WorldMinorErdtree"]),
        ("Portal", ["Portal"], []),
        ("Smithing Table", ["Smithing Table"], []),
        ("Wandering Mausoleum", ["Wandering Mausoleum"], []),
        ("Miquella's Cross", ["Miquella's Cross"], ["WorldMiquellaCross"]),
    ],
    "Key Items": [
        ("Cookbook", ["Cookbook"], ["KeyCookbooks"]),
        ("Great Rune", ["Great Rune"], ["KeyGreatRunes"]),
        ("Whetblade", ["Whetblade"], ["KeyWhetblades"]),
        ("Memory Stone", ["Memory Stone"], ["MagicMemoryStones"]),
        ("Spellbook", ["Spellbook"], ["MagicPrayerbooks"]),
        ("Map Fragment", ["Map Fragment"], ["WorldMaps"]),
        ("Painting", ["Painting"], ["WorldPaintings"]),
        ("Stonesword Key", ["Stonesword Key"], ["LootStoneswordKeys"]),
        ("Bell Bearing", ["Bell Bearing"], ["LootBellBearings", "LootMerchantBellBearings"]),
        ("Pots & Perfumes", ["Cracked Pot", "Ritual Pot", "Perfume Bottle"], ["KeyPotsNPerfumes"]),
        ("Scadutree Fragment", ["Scadutree Fragment"], ["KeyScadutreeFragments"]),
        ("Revered Spirit Ash", ["Revered Spirit Ash"], ["KeyLostAshes"]),
        ("Key Item (generic)", ["Key Item"], []),
        ("Remembrance", ["Remembrance"], []),
        ("Talisman Pouch", ["Talisman Pouch"], []),
        ("Tool", ["Tool"], []),
    ],
    "Items": [
        ("Ammunition", ["Ammunition"], ["LootAmmo"]),
        ("Consumable (all use-items)", ["Consumable"],
         ["LootConsumables", "LootGreases", "LootThrowables", "LootStatBoosts",
          "LootUtilities", "LootReusables", "LootPrattlingPates", "LootRadaFruit"]),
        ("Crystal Tear", ["Crystal Tear"], ["KeyCrystalTears"]),
        ("Deathroot", ["Deathroot"], ["QuestDeathroot"]),
        ("Dragon Heart", ["Dragon Heart"], ["LootDragonHearts"]),
        ("Gesture", ["Gesture"], ["LootGestures"]),
        ("Golden Rune", ["Golden Rune"], ["LootGoldenRunes", "LootGoldenRunesLow"]),
        ("Golden Seed + Sacred Tear", ["Golden Seed", "Sacred Tear"], ["KeySeedsTears"]),
        ("Larval Tear", ["Larval Tear"], ["KeyLarvalTears"]),
        ("Rune Arc", ["Rune Arc"], ["LootRuneArcs"]),
        ("Spirit Ashes", ["Spirit Ashes"], ["EquipSpirits"]),
        ("Multiplayer Item", ["Multiplayer Item"], ["LootMPFingers"]),
        ("Cerulean Scarab", ["Cerulean Scarab"], []),
        ("Crimson Scarab", ["Crimson Scarab"], []),
        ("Item (generic)", ["Item"], []),
    ],
    "Equipment": [
        ("Weapon + Shield", ["Weapon", "Shield"], ["EquipArmaments"]),
        ("Armor", ["Armor"], ["EquipArmour"]),
        ("Talisman", ["Talisman"], ["EquipTalismans"]),
        ("Ash of War", ["Ash of War"], ["EquipAshesOfWar"]),
        ("Sorcery", ["Sorcery"], ["MagicSorceries"]),
        ("Incantation", ["Incantation"], ["MagicIncantations"]),
    ],
    "NPCs": [
        ("Character", ["Character"], []),
        ("Ghost", ["Ghost"], []),
        ("Merchant", ["Merchant"], []),
        ("Trainer", ["Trainer"], []),
    ],
    "Enemies": [
        ("Bosses (all)", ["Boss", "Great Boss", "Legendary Boss"], ["WorldBosses"]),
        ("Invasion", ["Invasion"], ["WorldHostileNPC"]),
        ("Elite Enemy", ["Elite Enemy"], []),
        ("Enemy", ["Enemy"], []),
    ],
    "Other (guide annotations)": [
        ("Lore", ["Lore"], []),
        ("Miscellaneous", ["Miscellaneous"], []),
        ("Puzzle", ["Puzzle"], ["WorldInteractables"]),  # seals/chalices/statues ≈ MapGenie "Puzzle"
        ("Quest (steps)", ["Quest (steps)"], []),
        ("Stone Cairn", ["Stone Cairn"], []),
        ("Summoning Sigil", ["Summoning Sigil"], ["WorldSummoningPools"]),
    ],
    "Materials": [
        ("Smithing Stones (all)",
         ["Smithing Stone", "Ancient Smithing Stone", "Somber Smithing Stone"],
         ["LootSmithingStones", "LootSmithingStonesLow", "LootSmithingStonesRare"]),
        ("Crafting / gathering materials (lumped)",
         ["Crafting Material", "Glovewort", "Great Glovewort", "Ghost Glovewort",
          "Miquella's Lily", "Trina's Lily", "Ruin Fragment"],
         ["LootMaterialNodes", "LootCraftingMaterials", "LootGloveworts", "LootGreatGloveworts"]),
    ],
}

# Categories the mod draws but MapGenie has no row for here (so they don't read as "missing").
EXTRA_NOTE = {"Reforged - Rune Pieces", "Reforged - Ember Pieces", "Reforged - Fortunes",
              "Reforged - Items", "Key - Celestial Dew", "Key - Imbued Sword Keys",
              "Key - Lost Ashes", "Quest - Progression", "Quest - Seedbed Curses",
              "Loot - Rada Fruit", "World - Kindling Spirits", "World - Summoning Pools"}


def parse_coverage(log):
    """{display_name: drawn} from the latest [COVERAGE] block in the log."""
    rx = re.compile(r'\[COVERAGE\]\s+([A-Za-z][^=]+?)\s+baked=\d+\s+disk=\d+\s+live=\d+\s+'
                    r'live-cls=\d+\s+total=(\d+)')
    drawn = {}
    for line in open(log, encoding='utf-8', errors='replace'):
        m = rx.search(line)
        if m:
            drawn[m.group(1).strip()] = int(m.group(2))  # last wins → latest build
    return drawn


def status(mod, mg, wired):
    if not wired:
        return f"❌ NOT WIRED (MapGenie {mg})" if mg else "—"
    if mg == 0:
        return "—"
    if mod == 0:
        return f"⚪ WIRED-EMPTY (MapGenie {mg}, drew 0)"
    if mod < mg:
        return f"⚠️ DRIFT (−{mg - mod} vs MapGenie)"
    if mod > mg:
        return f"ℹ️ mod>MG (+{mod - mg}, ERR/taxonomy)"
    return "✅"


def main():
    log = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_LOG
    drawn = parse_coverage(log)
    if not drawn:
        sys.exit(f"no [COVERAGE] lines in {log} — run the game + open the map (diag not required; "
                 "[COVERAGE] is logged every build).")
    mg = {}
    for area in (60, 61):
        for k, v in MAPGENIE[area].items():
            mg[k] = mg.get(k, 0) + v

    def mod_drawn(cats):
        return sum(drawn.get(ENUM2DISPLAY.get(c, c), 0) for c in cats)

    used = set()
    L = ["# Coverage vs MapGenie — what the mod still doesn't show", "",
         "_Generated by tools/coverage_vs_mapgenie.py from the RUNTIME [COVERAGE] log (drawn = "
         "baked+disk+live). MapGenie = base(60)+DLC(61) summed (runtime [COVERAGE] is global)._", "",
         "**Legend:** ❌ NOT WIRED = no mod category for this MapGenie type (whole category unparsed) · "
         "⚪ WIRED-EMPTY = category exists but drew 0 · ⚠️ DRIFT = drawn < MapGenie · ℹ️ = mod shows more.",
         "", "> Graces are drawn by the always-on GraceLayer (NOT the g_buckets scoreboard), so the "
         "Site of Grace row reads WIRED-EMPTY here — it is actually shown. Known special case.", ""]
    not_wired, wired_empty, drift = [], [], []
    for section, rows in SECTIONS.items():
        present = [(d, labs, cats) for d, labs, cats in rows if any(l in mg for l in labs)]
        if not present:
            continue
        L += [f"## {section}", "", "| Type | MapGenie | mod drawn | Status |", "|---|--:|--:|---|"]
        for disp, labs, cats in present:
            used.update(cats)
            mgc = sum(mg.get(l, 0) for l in labs)
            wired = bool(cats)
            md = mod_drawn(cats) if wired else 0
            st = status(md, mgc, wired)
            if st.startswith("❌"): not_wired.append((section, disp, mgc))
            elif st.startswith("⚪"): wired_empty.append((section, disp, mgc))
            elif st.startswith("⚠️"): drift.append((section, disp, mgc - md))
            L.append(f"| {disp} | {mgc} | {md if wired else '—'} | {st} |")
        L.append("")

    # mod categories drawn but not mapped to any MapGenie row above
    mapped_disp = {ENUM2DISPLAY.get(c, c) for _, rows in SECTIONS.items() for _, _, cats in rows for c in cats}
    extra = sorted((n, v) for n, v in drawn.items() if n not in mapped_disp and v > 0)
    if extra:
        L += ["## Mod categories drawn but not in the MapGenie taxonomy", "",
              "_(ERR-specific or lumped differently — informational, not a gap.)_", "",
              "| Category | mod drawn |", "|---|--:|"]
        for n, v in extra:
            L.append(f"| {n} | {v} |")
        L.append("")

    L += ["## Summary — the gaps", "",
          f"- ❌ **NOT WIRED** (MapGenie types with no mod category): **{len(not_wired)}**",
          f"- ⚪ **WIRED-EMPTY** (category exists, drew 0): **{len(wired_empty)}**",
          f"- ⚠️ **DRIFT** (drawn < MapGenie): **{len(drift)}**", ""]
    if not_wired:
        L += ["### ❌ NOT WIRED — categories MapForGoblins does not parse at all", ""]
        for sec, disp, mgc in not_wired:
            L.append(f"- **{disp}** ({sec}) — MapGenie {mgc}")
        L.append("")
    if drift:
        L += ["### ⚠️ DRIFT — wired but fewer than MapGenie", ""]
        for sec, disp, gap in sorted(drift, key=lambda x: -x[2]):
            L.append(f"- **{disp}** ({sec}) — −{gap}")
        L.append("")

    DOCS.mkdir(exist_ok=True)
    out = DOCS / "coverage_vs_mapgenie.md"
    out.write_text("\n".join(L), encoding="utf-8")
    print(f"wrote {out.relative_to(ROOT)}  ({len(not_wired)} not-wired, {len(wired_empty)} wired-empty, "
          f"{len(drift)} drift; from {log.name})")


if __name__ == "__main__":
    main()
