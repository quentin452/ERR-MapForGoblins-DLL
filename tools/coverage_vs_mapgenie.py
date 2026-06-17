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


def mod_counts():
    txt = open(os.path.join(GEN, "goblin_map_data.cpp"), errors="replace").read()
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


def write_doc(area, counts, path, title):
    mg = MAPGENIE.get(area, {})
    used_cats = set()
    drift = notwired = 0

    L = [f"# {title} — mod vs MapGenie coverage", "",
         "_Generated by tools/coverage_vs_mapgenie.py._", "",
         "**Read the drift with care:**",
         "- MapGenie maps **vanilla** Elden Ring; this mod maps **ERR** (Reforged), "
         "which adds, removes and relocates items. So `mod < MapGenie` (⚠️) is a "
         "*candidate* gap, not proof — ERR may simply have fewer of that item. "
         "Investigate large negatives on stable types (cookbooks, stonesword keys, "
         "bell bearings) first; small ones are often ERR content differences.",
         "- Taxonomies differ. A `❌` row is a MapGenie type with no dedicated mod "
         "category — but some (Remembrance, generic Key Item) are still reachable via "
         "another marker (e.g. the boss), just not as a distinct icon.",
         "- ℹ️ `mod > MapGenie` is expected where ERR adds content.",
         ""]
    for section, rows in SECTIONS.items():
        present = [(disp, labs, cats) for disp, labs, cats in rows
                   if any(l in mg for l in labs)]
        if not present:
            continue
        L += [f"## {section}", "",
              "| Type | MapGenie | Mod | Status |", "|---|--:|--:|---|"]
        for disp, labs, cats in present:
            mgc = sum(mg.get(l, 0) for l in labs)
            modc = sum(counts.get(c, 0) for c in cats)
            used_cats.update(cats)
            wired = bool(cats)
            st = status(modc, mgc, wired)
            if st.startswith("⚠️"):
                drift += 1
            if st.startswith("❌"):
                notwired += 1
            L.append(f"| {disp} | {mgc} | {modc if wired else 0} | {st} |")
        L.append("")

    # mod categories not covered by any row above (MapGenie counts still TODO)
    rest = sorted([c for c in counts if c not in used_cats], key=lambda c: -counts[c])
    if rest:
        L += ["## Other mod categories (MapGenie counts TODO)", "",
              "| Category | Mod | MapGenie | Status |", "|---|--:|--:|---|"]
        for c in rest:
            L.append(f"| {c} | {counts[c]} | ? | — |")
        L.append("")

    L += ["## Summary", "",
          f"- mod markers on this map: **{sum(counts.values())}**",
          f"- MapGenie types not wired (❌): **{notwired}**",
          f"- wired types where mod < MapGenie (⚠️ drift): **{drift}**", ""]
    open(path, "w").write("\n".join(L))
    print(f"wrote {os.path.relpath(path, ROOT)}  "
          f"(markers {sum(counts.values())}, {notwired} unwired, {drift} drift)")


def main():
    os.makedirs(DOCS, exist_ok=True)
    counts = mod_counts()
    write_doc(60, counts[60], os.path.join(DOCS, "coverage_base.md"),
              "The Lands Between (base)")
    write_doc(61, counts[61], os.path.join(DOCS, "coverage_dlc.md"),
              "The Shadow Realm (DLC)")


if __name__ == "__main__":
    main()
