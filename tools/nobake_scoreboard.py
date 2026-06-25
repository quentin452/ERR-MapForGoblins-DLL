#!/usr/bin/env python3
"""Render docs/nobake_scoreboard.md from the runtime [COVERAGE] log.

Goal = ZERO baked: every marker should come from the live mod files (DiskMSB) or live
game memory (Live), never the static bake. This versioned doc is the baseline — after a
change, rerun + `git diff docs/nobake_scoreboard.md` to see regressions (baked ↑) or
progress (baked ↓). Rows are sorted by category NAME (stable) so a count change only
touches that row, never reorders the table.

Source: the latest `[COVERAGE]` block emitted by build_buckets_impl (one per map build).
Run the game with the overlay, open the map, then:
  py -3.14 tools/nobake_scoreboard.py [path\\to\\MapForGoblins.log]
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_LOG = (r"D:\DOWNLOAD\ERRv2.2.9.6-541-2-2-9-6-1780861369\ERRv2.2.9.6"
               r"\dll\offline\logs\MapForGoblins.log")
OUT = os.path.join(ROOT, "docs", "nobake_scoreboard.md")

ROW = re.compile(
    r"\[COVERAGE\]\s+(?P<cat>.+?)\s+baked=(?P<baked>\d+)\s+disk=(?P<disk>\d+)\s+"
    r"live=(?P<live>\d+)\s+live-cls=(?P<lc>\d+)\s+total=(?P<total>\d+)\s+\[(?P<status>[^\]]+)\]")
# Per-category census-vs-drawn + collect-flag coverage (emitted alongside the provenance row).
# drawn = real markers drawn; census = ImGui badge denominator (completable spots, EXCLUDES
# respawnable flag-less gather); flagged = markers carrying a collect/cleared flag; the rest
# split into respawn (lot-backed respawnable) + nonloot (TYPES with no collect flag).
CENSUS = re.compile(
    r"\[COVERAGE-CENSUS\]\s+(?P<cat>.+?)\s+drawn=(?P<drawn>\d+)\s+census=(?P<census>\d+)\s+"
    r"flagged=(?P<flagged>\d+)\s+respawn=(?P<respawn>\d+)\s+nonloot=(?P<nonloot>\d+)\s+total=\d+")
TS = re.compile(r"^\[(?P<ts>[\d\-: .]+)\]")
# [RESIDUAL-SRC] full provenance triage of the surviving baked-loot residual (build_buckets_impl):
# every baked loot row NOT replaced by a disk pass, tallied by its bake loot_source. This is WHY
# each leftover baked marker survives = the recovery lever per category. See the enemy investigation
# verdict below (memory msbe-enemy-loot-offsets): the `enemy` column is a CLOSED bake-mislabel, the
# `treasure` column is the corpse debake-gap, `emevd` is the one still-open recoverable lever.
RESID_TOT = re.compile(
    r"\[RESIDUAL-SRC\] surviving baked loot by source: unknown=(?P<unk>\d+) treasure=(?P<trea>\d+) "
    r"enemy=(?P<enem>\d+) emevd=(?P<emev>\d+) \(total=(?P<total>\d+)\)")
RESID_CAT = re.compile(
    r"\[RESIDUAL-SRC\]\s+(?P<cat>.+?): unk=(?P<unk>\d+) trea=(?P<trea>\d+) enem=(?P<enem>\d+) emev=(?P<emev>\d+)")

SRC = os.path.join(ROOT, "src")
COV_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "atlas_coverage.json")


def _slurp(*parts):
    try:
        return open(os.path.join(SRC, *parts), encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def icon_metadata():
    """Static per-category map-icon info, parsed from the C++ sources + the embedded atlas.

    Returns {display_name: desc}. desc is one of:
      symbol     -> native GPU map symbol (category_gpu_icon_name, e.g. Bosses); always crisp.
      atlas N%   -> baked atlas CPU cell, N% non-transparent coverage. A trailing ⚠ marks a
                    FAINT/small cell (<25%) — the icon renders but is easy to miss on the busy
                    map (this is the Stakes-of-Marika class of "looks broken but isn't"); bump
                    overlay_icon_scale or enlarge the source icon.
      circle     -> no atlas key (nullptr in the CAT table) -> a flat coloured circle is drawn.
      none       -> a key is set but absent from the atlas -> falls back to the circle.
    NOTE: item categories (Equipment/Key/Loot/Magic/Reforged items) ALSO draw the game's live
    GPU inventory icon when the item is resident; their atlas N% is only the fallback, so a low
    % matters most for the atlas-only World features.

    Atlas coverage needs Pillow. When present it's computed AND cached to tools/atlas_coverage.json
    so a no-PIL run (e.g. py 3.14) still shows the % from the cache.
    """
    import json
    enum2disp = {m.group(1): m.group(2) for m in
                 re.finditer(r'case\s+C::(\w+):\s*return\s+"([^"]+)";', _slurp("goblin_markers.cpp"))}
    cm = _slurp("worldmap", "category_meta.cpp")
    enum2key = {}  # CAT[] entries:  {"key", G_X},  // EnumName   (or {nullptr, G_X}, // EnumName)
    for m in re.finditer(r'\{\s*(nullptr|"([^"]*)")\s*,\s*G_\w+\s*\}\s*,\s*//\s*(\w+)', cm):
        enum2key[m.group(3)] = None if m.group(1) == "nullptr" else m.group(2)
    gpu = {m.group(1) for m in re.finditer(r'static_cast<int>\(C::(\w+)\)\s*,\s*"[^"]+"', cm)}
    oi = _slurp("generated_shared", "goblin_overlay_icons.cpp")
    cells = {m.group(1): (int(m.group(2)), int(m.group(3)))
             for m in re.finditer(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}', oi)}
    cov = None
    try:
        from PIL import Image
        import io
        cell = int(re.search(r'\bCELL\s*=\s*(\d+)', oi).group(1))
        body = re.search(r'ATLAS_PNG\[\]\s*=\s*\{(.*?)\};', oi, re.S).group(1)
        vals = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body)]
        img = Image.open(io.BytesIO(bytes(v & 0xff for v in vals))).convert("RGBA")
        cov = {}
        for key, (c, r) in cells.items():
            a = list(img.crop((c * cell, r * cell, (c + 1) * cell, (r + 1) * cell))
                     .getchannel("A").getdata())
            cov[key] = round(100 * sum(1 for v in a if v > 8) / len(a))
        with open(COV_CACHE, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(cov, fh, indent=0, sort_keys=True)
    except Exception:
        try:
            cov = json.load(open(COV_CACHE, encoding="utf-8"))
        except OSError:
            cov = None

    def desc(enum):
        if enum in gpu:
            return "symbol"
        key = enum2key.get(enum, "__nokey__")
        if key is None:
            return "circle"
        if key not in cells:
            return "none"
        if cov is None:
            return "atlas"
        pct = cov.get(key, 0)
        return f"atlas {pct}%" + (" ⚠" if pct < 25 else "")

    return {disp: desc(enum) for enum, disp in enum2disp.items()}


TILES = re.compile(
    r"\[COVERAGE-TILES\] parsed (?P<parsed>\d+)/(?P<total>\d+) tiles.*?"
    r"skipped (?P<skipped>\d+) non-_00 \[(?P<bytier>[^\]]*)\]")


def parse_last_block(path):
    rows, ts, tiles = {}, None, None
    resid, resid_tot = {}, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = TS.match(line)
            block_ts = t.group("ts") if t else None
            tl = TILES.search(line)
            if tl:
                tiles = {"parsed": int(tl.group("parsed")), "total": int(tl.group("total")),
                         "skipped": int(tl.group("skipped")), "bytier": tl.group("bytier")}
            rt = RESID_TOT.search(line)
            if rt:
                resid_tot = {k: int(rt.group(k)) for k in ("unk", "trea", "enem", "emev", "total")}
                resid = {}  # a fresh [RESIDUAL-SRC] block starts → reset the per-cat accumulation
                continue
            rc = RESID_CAT.search(line)
            if rc:
                resid[rc.group("cat").strip()] = {k: int(rc.group(k))
                                                  for k in ("unk", "trea", "enem", "emev")}
                continue
            m = ROW.search(line)
            if m:
                cat = m.group("cat").strip()
                # A new scoreboard block: each build re-emits every category. Keep the LAST
                # value seen per category — later lines win, so the final pass over the file
                # naturally yields the most recent build's numbers.
                rows.setdefault(cat, {}).update(
                    {k: (int(m.group(k)) if k != "status" else m.group(k))
                     for k in ("baked", "disk", "live", "lc", "total", "status")})
                ts = block_ts or ts
                continue
            c = CENSUS.search(line)
            if c:
                cat = c.group("cat").strip()
                rows.setdefault(cat, {}).update(
                    {k: int(c.group(k))
                     for k in ("drawn", "census", "flagged", "respawn", "nonloot")})
                ts = block_ts or ts
    return rows, ts, tiles, resid, resid_tot


def emit(rows, ts, tiles=None, resid=None, resid_tot=None):
    tot = {k: sum(r[k] for r in rows.values()) for k in ("baked", "disk", "live", "lc", "total")}
    icons = icon_metadata()

    def badge(r):
        if r["baked"] == 0 and (r["disk"] or r["live"]):
            return "🟢 off-bake"
        if r["disk"] == 0 and r["live"] == 0:
            return "🔴 baked-only"
        return "🟡 partial"

    lines = []
    lines.append("# No-bake scoreboard — markers by provenance")
    lines.append("")
    lines.append("**Goal: zero baked.** Every marker should come from the live mod files "
                 "(`DiskMSB`) or live game memory (`Live`), never the static `goblin_map_data` "
                 "bake. This doc is the versioned baseline — after a change, rerun "
                 "`tools/nobake_scoreboard.py` and `git diff` this file to see **regressions "
                 "(baked ↑)** or **progress (baked ↓)**. Rows sorted by category name (stable) "
                 "so a count change touches only its own row.")
    lines.append("")
    lines.append("- **Source**: runtime `[COVERAGE]` log (ERR profile)" + (f", build {ts}" if ts else ""))
    lines.append("- **`live-cls`** = category resolved via the live `classify_item_live` fallback "
                 "(item the baked table didn't know).")
    lines.append("- `disk`/`live` counts are **per-placement** (collectibles emit one marker per "
                 "world node) → `total` is not directly comparable to deduped baked counts. For "
                 "the migration what matters is **does a category still have baked>0**.")
    lines.append("- **`drawn`** = real markers the renderer draws (= total). **`census`** = the "
                 "ImGui badge denominator (completable spots) — distinct collect flags for "
                 "flag-based categories, row count for geom/SFX pieces, 0 for graces; it EXCLUDES "
                 "respawnable flag-less gather, so `census < drawn` wherever markers share a flag "
                 "or respawn.")
    lines.append("- **`flag`** = collect-flag coverage `flagged/drawn`: markers carrying a "
                 "collect/cleared flag (can be collect-tracked / gray out) vs not. The flag-less "
                 "split into **`respawn`** (lot-backed respawnable gather, no permanent done-state) "
                 "and **`nonloot`** (TYPES with no collect flag — NPC/stake/spring/region). A big "
                 "`nonloot` flags a feature whose objects can never complete on the map.")
    lines.append("- **`icon`** = how the category's marker is drawn: **`symbol`** = native GPU map "
                 "symbol (crisp); **`atlas N%`** = baked atlas CPU cell, N% non-transparent (a "
                 "trailing ⚠ = faint/small <25%, renders but easy to miss — the Stakes class of "
                 "\"looks broken but isn't\"; bump `overlay_icon_scale`); **`circle`** = flat "
                 "coloured disc; **`none`** = key set but absent from the atlas. Item categories "
                 "(Equipment/Key/Loot/Magic) also draw the live GPU inventory icon, so their "
                 "atlas % is only the fallback — low % matters most for atlas-only World features.")
    lines.append("- Graces are `Live` (BonfireWarpParam) but tallied separately in GraceLayer — "
                 "not in this table.")
    lines.append("")
    lines.append("## ▶ Baked markers remaining")
    lines.append("")
    lines.append(f"# **{tot['baked']}**  ← drive this to **0**")
    lines.append("")
    lines.append(f"| | baked | disk | live | live-cls | total |")
    lines.append(f"|---|--:|--:|--:|--:|--:|")
    lines.append(f"| **all categories** | **{tot['baked']}** | {tot['disk']} | {tot['live']} "
                 f"| {tot['lc']} | {tot['total']} |")
    nbaked = sum(1 for r in rows.values() if r["disk"] == 0 and r["live"] == 0)
    npart = sum(1 for r in rows.values() if r["baked"] and (r["disk"] or r["live"]))
    ngreen = sum(1 for r in rows.values() if r["baked"] == 0 and (r["disk"] or r["live"]))
    lines.append("")
    lines.append(f"🔴 baked-only: **{nbaked}**  ·  🟡 partial: **{npart}**  ·  🟢 off-bake: **{ngreen}**  "
                 f"(of {len(rows)} active categories)")
    lines.append("")
    if tiles:
        lines.append("## Tile coverage (`_00`-only parser)")
        lines.append("")
        lines.append(f"The disk pass parses only **`_00`** tiles (LOD0). It reads "
                     f"**{tiles['parsed']} / {tiles['total']}** tiles; the **{tiles['skipped']}** "
                     f"non-`_00` are skipped. `_01`/`_02` are LOD connect-proxies (proxy objects at "
                     f"a 128/256 offset → the Stakes/Imp phantom source); `_10`/`_11`/`_12` hold "
                     f"mostly GED-tier **duplicates** of `_00` (e.g. the 3 Hostile-NPC invaders the "
                     f"bake double-counted), so skipping them loses ~no unique markers. "
                     f"`tools/tier_coverage.py` audits per-tier unique content.")
        lines.append("")
        lines.append("| tier | files | role |")
        lines.append("|---|--:|---|")
        ROLE = {0: "parsed (LOD0 content)", 1: "skipped — LOD proxy", 2: "skipped — LOD proxy",
                10: "skipped — mostly _00 dupes", 11: "skipped — mostly _00 dupes",
                12: "skipped — mostly _00 dupes", 99: "skipped — lighting"}
        for tok in tiles["bytier"].split():
            try:
                key, n = tok.split("=")
                lod = int(key.lstrip("_"))
            except ValueError:
                continue
            lines.append(f"| `{key}` | {n} | {ROLE.get(lod, 'skipped')} |")
        lines.append("")
    lines.append("## Per category")
    lines.append("")
    lines.append("| category | baked | disk | live | live-cls | total | icon | status |")
    lines.append("|---|--:|--:|--:|--:|--:|---|---|")
    for cat in sorted(rows, key=str.lower):
        r = rows[cat]
        lines.append(f"| {cat} | {r['baked']} | {r['disk']} | {r['live']} | {r['lc']} "
                     f"| {r['total']} | {icons.get(cat, '?')} | {badge(r)} |")
    lines.append("")
    if resid:
        lines.append("## Baked residual by provenance — why each baked marker survives")
        lines.append("")
        lines.append("Every surviving baked **loot** row (not replaced by any disk pass), tallied by its "
                     "bake `loot_source`. This is the **recovery lever per category** — and the result of "
                     "the 2026-06-25 deep-dive into the residual (see `memory/msbe-enemy-loot-offsets.md`):")
        lines.append("")
        lines.append("- **`treasure`** — an MSB Treasure the disk pass didn't reproduce: the **corpse "
                     "debake-gap**, items whose ItemLotParam chain is absent from the mod's loot linkage. "
                     "Accepted residual (~0.4% of the treasure slice).")
        lines.append("- **`enemy`** — ✅ **INVESTIGATED & CLOSED: a bake MIS-LABEL, not recoverable here.** "
                     "These lots are referenced by **NO `NpcParam.itemLotId`** — proven irrefutably against "
                     "every parsed enemy, the FULL NpcParam table, the paramdef-authoritative offline scan, "
                     "AND the **vanilla** regulation (all **0** matches). mapgenie shows them on *corpses/"
                     "bodies* → they are corpse/EMEVD-scripted loot the bake wrongly tagged `Enemy`. The "
                     "NpcParam enemy pass is COMPLETE; the items appear elsewhere via the treasure/emevd "
                     "passes (or are phantom dupes). A bake regen would re-tag them.")
        lines.append("- **`emevd`** — an EMEVD award the disk EMEVD pass didn't reproduce: a **still-open, "
                     "genuinely recoverable** lever (extend the EMEVD template coverage).")
        lines.append("- **`unknown`** — pre-provenance bake rows (the `loot_source` field predates the "
                     "tagging and wasn't regenerated); could be any source. A regen reclassifies them.")
        lines.append("")
        if resid_tot:
            lines.append(f"Residual loot total **{resid_tot['total']}** = unknown {resid_tot['unk']} · "
                         f"treasure {resid_tot['trea']} (accepted) · enemy {resid_tot['enem']} "
                         f"(bake mis-label) · emevd {resid_tot['emev']} (recoverable).")
            lines.append("")
        lines.append("| category | unknown | treasure (accepted) | enemy (mis-label) | emevd (recoverable) |")
        lines.append("|---|--:|--:|--:|--:|")
        for cat in sorted(resid, key=str.lower):
            d = resid[cat]
            lines.append(f"| {cat} | {d['unk']} | {d['trea']} | {d['enem']} | {d['emev']} |")
        lines.append("")
    lines.append("## Census (badge vs drawn) + collect-flag coverage")
    lines.append("")
    lines.append("`drawn` = markers drawn · `census` = completable-spots badge · "
                 "`flag` = flagged/drawn · `respawn`/`nonloot` = the flag-less split. "
                 "A large `nonloot` marks a feature whose objects carry no collect flag "
                 "(can't gray/complete). Categories missing here weren't in the census log.")
    lines.append("")
    lines.append("| category | drawn | census | flag (have/drawn) | respawn | nonloot |")
    lines.append("|---|--:|--:|--:|--:|--:|")
    for cat in sorted(rows, key=str.lower):
        r = rows[cat]
        if "drawn" not in r:
            continue
        lines.append(f"| {cat} | {r['drawn']} | {r['census']} | {r['flagged']}/{r['drawn']} "
                     f"| {r['respawn']} | {r['nonloot']} |")
    lines.append("")
    return "\n".join(lines) + "\n"


def main():
    log = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_LOG
    if not os.path.isfile(log):
        sys.exit(f"log not found: {log}\nPass the path as arg 1.")
    rows, ts, tiles, resid, resid_tot = parse_last_block(log)
    if not rows:
        sys.exit("no [COVERAGE] rows found in the log — run the game + open the map first.")
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(emit(rows, ts, tiles, resid, resid_tot))
    print(f"wrote {OUT}  ({len(rows)} categories, baked remaining = "
          f"{sum(r['baked'] for r in rows.values())})")


if __name__ == "__main__":
    main()
