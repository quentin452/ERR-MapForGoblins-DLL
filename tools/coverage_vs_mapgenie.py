#!/usr/bin/env python3
"""Generate per-map coverage docs: mod marker counts vs MapGenie counts, drift-flagged.

Writes docs/coverage_base.md (area 60) and docs/coverage_dlc.md (area 61). For
each comparison row it shows MapGenie's count, the mod's count, and a status —
the important one being ⚠️ when mod < MapGenie (a real gap). mod > MapGenie is
expected (ERR adds content) and only noted.

MapGenie numbers are hand-entered from the site's category panels. Mapping is
explicit (SECTIONS) because the taxonomies differ: one mod category can cover
several MapGenie types (Pots & Perfumes) and vice-versa (Bell Bearing). Rows
with an empty mod-category list are MapGenie types with no mod equivalent.

Usage:  tools/coverage_vs_mapgenie.py
"""
import os
import re
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "src", "generated")
DOCS = os.path.join(ROOT, "docs")

# ── MapGenie counts per area, flat by label (across all sections) ─────────────
MAPGENIE = {
    60: {  # The Lands Between
        # Locations
        "Site of Grace": 309, "Stake of Marika": 97, "Spiritspring Jump": 38,
        "Imp Seal Statue": 38, "Divine Tower": 6, "Dragon Shrine": 2,
        "Dungeon": 53, "Elevator": 24, "Evergaol": 11, "Hidden Passage": 40,
        "Landmark": 131, "Legacy Dungeon": 7, "Martyr Effigy": 156,
        "Minor Erdtree": 11, "Portal": 36, "Smithing Table": 1,
        "Wandering Mausoleum": 7,
        # Key Items
        "Bell Bearing": 52, "Cookbook": 59, "Cracked Pot": 14, "Great Rune": 10,
        "Key Item": 68, "Map Fragment": 19, "Memory Stone": 8, "Painting": 7,
        "Perfume Bottle": 10, "Remembrance": 15, "Ritual Pot": 9, "Spellbook": 11,
        "Stonesword Key": 65, "Talisman Pouch": 3, "Tool": 23, "Whetblade": 6,
        # Items
        "Ammunition": 98, "Cerulean Scarab": 21, "Consumable": 265,
        "Crimson Scarab": 40, "Crystal Tear": 32, "Deathroot": 9,
        "Dragon Heart": 13, "Gesture": 35, "Golden Rune": 407, "Golden Seed": 42,
        "Item": 9, "Larval Tear": 17, "Multiplayer Item": 34, "Rune Arc": 63,
        "Sacred Tear": 12, "Spirit Ashes": 65,
        # Equipment
        "Armor": 164, "Ash of War": 105, "Incantation": 101, "Shield": 69,
        "Sorcery": 73, "Talisman": 122, "Weapon": 315,
        # NPCs
        "Character": 109, "Ghost": 36, "Merchant": 40, "Trainer": 1,
        # Enemies
        "Boss": 140, "Elite Enemy": 144, "Enemy": 58, "Great Boss": 14,
        "Invasion": 36, "Legendary Boss": 16,
    },
    61: {  # The Shadow Realm (DLC)
        # Locations
        "Site of Grace": 105, "Stake of Marika": 22, "Spiritspring Jump": 18,
        "Dungeon": 11, "Elevator": 16, "Hidden Passage": 19, "Landmark": 41,
        "Martyr Effigy": 56, "Miquella's Cross": 13, "Portal": 3,
        # Key Items
        "Bell Bearing": 10, "Cookbook": 45, "Cracked Pot": 10, "Great Rune": 1,
        "Key Item": 17, "Map Fragment": 5, "Painting": 3, "Remembrance": 10,
        "Revered Spirit Ash": 23, "Scadutree Fragment": 42, "Tool": 7,
        # Items
        "Ammunition": 22, "Consumable": 96, "Crystal Tear": 8, "Dragon Heart": 6,
        "Gesture": 4, "Golden Rune": 55, "Item": 4, "Larval Tear": 9,
        "Multiplayer Item": 3, "Rune Arc": 5, "Spirit Ashes": 20,
        # Equipment
        "Armor": 46, "Ash of War": 23, "Incantation": 28, "Shield": 11,
        "Sorcery": 14, "Talisman": 40, "Weapon": 92,
        # NPCs
        "Character": 18, "Ghost": 21, "Merchant": 3,
        # Enemies
        "Boss": 33, "Elite Enemy": 40, "Enemy": 24, "Great Boss": 1,
        "Invasion": 13, "Legendary Boss": 10,
    },
}

# Sections: each row = (display, [MapGenie labels to sum], [mod Category enums to sum]).
# Empty mod list = MapGenie type with no mod equivalent (not wired).
SECTIONS = {
    "Locations": [
        ("Site of Grace", ["Site of Grace"], ["WorldGraces"]),
        ("Stake of Marika", ["Stake of Marika"], ["WorldStakesOfMarika"]),
        ("Spiritspring Jump", ["Spiritspring Jump"],
         ["WorldSpiritSprings", "WorldSpiritspringHawks"]),
        ("Imp Seal Statue", ["Imp Seal Statue"], ["WorldImpStatues"]),
        ("Divine Tower", ["Divine Tower"], []),
        ("Dragon Shrine", ["Dragon Shrine"], []),
        ("Dungeon", ["Dungeon"], []),
        ("Elevator", ["Elevator"], []),
        ("Evergaol", ["Evergaol"], []),
        ("Hidden Passage", ["Hidden Passage"], []),
        ("Landmark", ["Landmark"], []),
        ("Legacy Dungeon", ["Legacy Dungeon"], []),
        ("Martyr Effigy", ["Martyr Effigy"], []),
        ("Minor Erdtree", ["Minor Erdtree"], []),
        ("Portal", ["Portal"], []),
        ("Smithing Table", ["Smithing Table"], []),
        ("Wandering Mausoleum", ["Wandering Mausoleum"], []),
        ("Miquella's Cross", ["Miquella's Cross"], []),
    ],
    "Key Items": [
        ("Cookbook", ["Cookbook"], ["KeyCookbooks"]),
        ("Great Rune", ["Great Rune"], ["KeyGreatRunes"]),
        ("Whetblade", ["Whetblade"], ["KeyWhetblades"]),
        ("Memory Stone", ["Memory Stone"], ["MagicMemoryStones"]),
        ("Spellbook", ["Spellbook"], ["MagicPrayerbooks"]),  # ~ uncertain mapping
        ("Map Fragment", ["Map Fragment"], ["WorldMaps"]),
        ("Painting", ["Painting"], ["WorldPaintings"]),
        ("Stonesword Key", ["Stonesword Key"], ["LootStoneswordKeys"]),
        ("Bell Bearing", ["Bell Bearing"],
         ["LootBellBearings", "LootMerchantBellBearings"]),
        ("Pots & Perfumes", ["Cracked Pot", "Ritual Pot", "Perfume Bottle"],
         ["KeyPotsNPerfumes"]),
        ("Scadutree Fragment", ["Scadutree Fragment"], ["KeyScadutreeFragments"]),
        # MapGenie types with no clean mod equivalent:
        ("Key Item (generic)", ["Key Item"], []),
        ("Remembrance", ["Remembrance"], []),
        ("Talisman Pouch", ["Talisman Pouch"], []),
        ("Tool", ["Tool"], []),
        ("Revered Spirit Ash", ["Revered Spirit Ash"], []),
    ],
    "Items": [
        ("Ammunition", ["Ammunition"], ["LootAmmo"]),
        # MapGenie lumps all use-items as "Consumable"; the mod splits them, so
        # sum the consumable family for a fair compare (still noisy vs ERR).
        ("Consumable (all use-items)", ["Consumable"],
         ["LootConsumables", "LootGreases", "LootThrowables", "LootStatBoosts",
          "LootUtilities", "LootReusables", "LootPrattlingPates"]),
        ("Crystal Tear", ["Crystal Tear"], ["KeyCrystalTears"]),
        ("Deathroot", ["Deathroot"], ["QuestDeathroot"]),
        ("Dragon Heart", ["Dragon Heart"], ["LootDragonHearts"]),
        ("Gesture", ["Gesture"], ["LootGestures"]),
        ("Golden Rune", ["Golden Rune"], ["LootGoldenRunes", "LootGoldenRunesLow"]),
        ("Golden Seed + Sacred Tear", ["Golden Seed", "Sacred Tear"],
         ["KeySeedsTears"]),
        ("Larval Tear", ["Larval Tear"], ["KeyLarvalTears"]),
        ("Rune Arc", ["Rune Arc"], ["LootRuneArcs"]),
        ("Spirit Ashes", ["Spirit Ashes"], ["EquipSpirits"]),
        # MapGenie types with no clean mod equivalent:
        ("Cerulean Scarab", ["Cerulean Scarab"], []),
        ("Crimson Scarab", ["Crimson Scarab"], []),
        ("Item (generic)", ["Item"], []),
        ("Multiplayer Item", ["Multiplayer Item"], ["LootMPFingers"]),
    ],
    "Equipment": [
        # mod "Armaments" = weapons + shields + staves + seals; MapGenie splits
        # Weapon (incl staves/seals) and Shield, so compare the sum.
        ("Weapon + Shield", ["Weapon", "Shield"], ["EquipArmaments"]),
        ("Armor", ["Armor"], ["EquipArmour"]),
        ("Talisman", ["Talisman"], ["EquipTalismans"]),
        ("Ash of War", ["Ash of War"], ["EquipAshesOfWar"]),
        ("Sorcery", ["Sorcery"], ["MagicSorceries"]),
        ("Incantation", ["Incantation"], ["MagicIncantations"]),
    ],
    "NPCs": [
        # The mod has no friendly-NPC / merchant / ghost / trainer category;
        # WorldHostileNPC (invaders) is a different thing and stays in "Other".
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
}

GROUPS = ["Equip", "Loot", "Key", "Magic", "World", "Reforged", "Quest"]


def parse_legacy_conv():
    txt = open(os.path.join(GEN, "goblin_legacy_conv.hpp")).read()
    rows = []
    for m in re.finditer(
        r"\{\s*(\d+),\s*(\d+),\s*[-\d.]+f,\s*[-\d.]+f,\s*(\d+),", txt):
        rows.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return rows


CONV = parse_legacy_conv()


def dst_area(area, gx):
    if area in (60, 61):
        return area
    fb = None
    for sa, sgx, da in CONV:
        if sa != area:
            continue
        if fb is None:
            fb = da
        if sgx == gx:
            return da
    return fb


# Build profiles MapForGoblins targets, in display order: (label, generated dir).
PROFILES = [("vanilla", "generated_vanilla"), ("ERR", "generated"),
            ("conv", "generated_convergence"), ("erte", "generated_erte")]


def available_profiles():
    out = []
    for label, sub in PROFILES:
        if os.path.exists(os.path.join(ROOT, "src", sub, "goblin_map_data.cpp")):
            out.append((label, sub))
    return out


def mod_counts(sub):
    """Per-area Counter(category->n) for one profile's generated data dir."""
    path = os.path.join(ROOT, "src", sub, "goblin_map_data.cpp")
    txt = open(path, errors="replace").read()
    out = {60: Counter(), 61: Counter()}
    for blk in txt.split("// Row ID ")[1:]:
        a = re.search(r"\.areaNo\s*=\s*(\d+)", blk)
        gx = re.search(r"\.gridXNo\s*=\s*(\d+)", blk)
        cat = re.search(r"Category::(\w+)", blk)
        if not (a and cat):
            continue
        da = dst_area(int(a.group(1)), int(gx.group(1)) if gx else 0)
        if da in out:
            out[da][cat.group(1)] += 1
    return out


def group_of(cat):
    for g in GROUPS:
        if cat.startswith(g):
            return g
    return "Other"


def status(mod, mg, wired):
    if not wired:
        return f"❌ NOT WIRED (need {mg})" if mg else "—"
    if mg is None or mg == 0:
        return "—"
    if mod < mg:
        return f"⚠️ DRIFT mod<MG (−{mg - mod})"
    if mod > mg:
        return f"ℹ️ mod>MG (+{mod - mg}, ERR?)"
    return "✅"


def write_doc(area, prof_counts, profiles, path, title):
    """prof_counts = {profile_label: Counter} for this area; profiles = [(label,sub)]."""
    mg = MAPGENIE.get(area, {})
    labels = [lab for lab, _ in profiles]
    # baseline for the bug flag: vanilla (same game as MapGenie) if present, else ERR.
    base = "vanilla" if "vanilla" in labels else ("ERR" if "ERR" in labels else labels[0])
    used_cats = set()
    drift = notwired = 0

    def modsum(label, cats):
        return sum(prof_counts[label].get(c, 0) for c in cats)

    hdr = "| Type | MapGenie | " + " | ".join(labels) + " | Status |"
    sep = "|---|--:|" + "--:|" * len(labels) + "---|"

    L = [f"# {title} — mod vs MapGenie coverage", "",
         "_Generated by tools/coverage_vs_mapgenie.py._", "",
         f"Mod profiles present: **{', '.join(labels)}**. Bug flag (Status) compares "
         f"**{base}** to MapGenie.",
         "",
         "**Reading it:**",
         f"- The clean bug signal is **vanilla vs MapGenie** (same game). If the "
         f"vanilla column is present, ⚠️ there = a real mod gap. (Baseline here: {base}.)",
         "- **ERR/conv/erte minus vanilla** = what that overhaul added/removed "
         "(feature, not bug). Without a vanilla column, ⚠️ vs ERR is only a *candidate* "
         "(ERR ≠ vanilla content).",
         "- ❌ = MapGenie type with no dedicated mod category (some still reachable "
         "via another marker, e.g. boss remembrances). ℹ️ = mod has more.",
         ""]
    for section, rows in SECTIONS.items():
        present = [(disp, labs, cats) for disp, labs, cats in rows
                   if any(l in mg for l in labs)]
        if not present:
            continue
        L += [f"## {section}", "", hdr, sep]
        for disp, labs, cats in present:
            mgc = sum(mg.get(l, 0) for l in labs)
            used_cats.update(cats)
            wired = bool(cats)
            cells = " | ".join(str(modsum(lab, cats) if wired else 0) for lab in labels)
            st = status(modsum(base, cats), mgc, wired)
            if st.startswith("⚠️"):
                drift += 1
            if st.startswith("❌"):
                notwired += 1
            L.append(f"| {disp} | {mgc} | {cells} | {st} |")
        L.append("")

    rest = sorted({c for lab in labels for c in prof_counts[lab]} - used_cats,
                  key=lambda c: -prof_counts[base].get(c, 0))
    if rest:
        L += ["## Other mod categories (MapGenie counts TODO)", "",
              "| Category | MapGenie | " + " | ".join(labels) + " |",
              "|---|--:|" + "--:|" * len(labels) + ""]
        for c in rest:
            cells = " | ".join(str(prof_counts[lab].get(c, 0)) for lab in labels)
            L.append(f"| {c} | ? | {cells} |")
        L.append("")

    L += ["## Summary", "",
          f"- mod markers ({base}) on this map: **{sum(prof_counts[base].values())}**",
          f"- MapGenie types not wired (❌): **{notwired}**",
          f"- wired types where {base} < MapGenie (⚠️): **{drift}**", ""]
    open(path, "w").write("\n".join(L))
    print(f"wrote {os.path.relpath(path, ROOT)}  "
          f"(profiles {','.join(labels)}, {notwired} unwired, {drift} drift vs {base})")


def main():
    os.makedirs(DOCS, exist_ok=True)
    profiles = available_profiles()
    counts = {lab: mod_counts(sub) for lab, sub in profiles}
    for area, fname, t in [(60, "coverage_base.md", "The Lands Between (base)"),
                           (61, "coverage_dlc.md", "The Shadow Realm (DLC)")]:
        pc = {lab: counts[lab][area] for lab in [l for l, _ in profiles]}
        write_doc(area, pc, profiles, os.path.join(DOCS, fname), t)


if __name__ == "__main__":
    main()
