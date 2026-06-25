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


def parse_last_block(path):
    rows, ts = {}, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = TS.match(line)
            block_ts = t.group("ts") if t else None
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
    return rows, ts


def emit(rows, ts):
    tot = {k: sum(r[k] for r in rows.values()) for k in ("baked", "disk", "live", "lc", "total")}

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
    lines.append("## Per category")
    lines.append("")
    lines.append("| category | baked | disk | live | live-cls | total | status |")
    lines.append("|---|--:|--:|--:|--:|--:|---|")
    for cat in sorted(rows, key=str.lower):
        r = rows[cat]
        lines.append(f"| {cat} | {r['baked']} | {r['disk']} | {r['live']} | {r['lc']} "
                     f"| {r['total']} | {badge(r)} |")
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
    rows, ts = parse_last_block(log)
    if not rows:
        sys.exit("no [COVERAGE] rows found in the log — run the game + open the map first.")
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(emit(rows, ts))
    print(f"wrote {OUT}  ({len(rows)} categories, baked remaining = "
          f"{sum(r['baked'] for r in rows.values())})")


if __name__ == "__main__":
    main()
