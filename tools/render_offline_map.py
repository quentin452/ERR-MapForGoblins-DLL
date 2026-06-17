#!/usr/bin/env python3
"""Offline full-map render of every MapForGoblins marker — no game, no fog, no save risk.

Projects each baked marker onto the overworld plane with the SAME logic the DLL
uses at inject (LEGACY_CONV exact match, then same-src_area fallback), then emits
a self-contained HTML page: a zoomable map (inline SVG) + a MapGenie-style sidebar
of category filters (grouped, with counts) you can toggle on/off to instantly see
where a given category is dense or missing. No game, no fog of war, no save risk.

Usage:  tools/render_offline_map.py [out.html] [--area 60|61] [--map m10_01]
        --area : overworld plane (60 = Lands Between, 61 = Shadow Realm/DLC),
                 dungeons projected onto it (mirrors the in-game map).
        --map  : render ONE dungeon in its own local coords (its real interior
                 layout, like a MapGenie submap) — no projection, no clustering.
                 e.g. --map m10_01 = Fringefolk Hero's Grave.
"""
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "src", "generated")

# Macro-groups (MapGenie-style) by category-name prefix, in display order.
GROUPS = ["Equip", "Loot", "Key", "Magic", "World", "Reforged", "Quest"]


def group_of(cat):
    for g in GROUPS:
        if cat.startswith(g):
            return g
    return "Other"


def parse_legacy_conv():
    txt = open(os.path.join(GEN, "goblin_legacy_conv.hpp")).read()
    rows = []
    for m in re.finditer(
        r"\{\s*(\d+),\s*(\d+),\s*([-\d.]+)f,\s*([-\d.]+)f,\s*"
        r"(\d+),\s*(\d+),\s*(\d+),\s*([-\d.]+)f,\s*([-\d.]+)f\s*\}", txt):
        g = m.groups()
        rows.append(dict(src_area=int(g[0]), src_gx=int(g[1]),
                         src_pos_x=float(g[2]), src_pos_z=float(g[3]),
                         dst_area=int(g[4]), dst_gx=int(g[5]), dst_gz=int(g[6]),
                         dst_pos_x=float(g[7]), dst_pos_z=float(g[8])))
    return rows


CONV = parse_legacy_conv()


def project(area, gx, gz, px, pz):
    """Replicate the DLL: overworld passthrough, else exact (area,gx) then same-area fallback."""
    if area in (60, 61):
        return area, gx * 256.0 + px, gz * 256.0 + pz
    exact = area_fb = None
    for c in CONV:
        if c["src_area"] != area:
            continue
        if area_fb is None:
            area_fb = c
        if c["src_gx"] == gx:
            exact = c
            break
    c = exact or area_fb
    if not c:
        return None
    wx = c["dst_gx"] * 256.0 + c["dst_pos_x"]
    wz = c["dst_gz"] * 256.0 + c["dst_pos_z"]
    if exact:
        wx += px - c["src_pos_x"]
        wz += pz - c["src_pos_z"]
    return c["dst_area"], wx, wz


def parse_markers():
    txt = open(os.path.join(GEN, "goblin_map_data.cpp"), errors="replace").read()
    out = []
    for blk in txt.split("// Row ID ")[1:]:
        def f(name, cast):
            m = re.search(r"\.%s\s*=\s*([-\d.]+)" % name, blk)
            return cast(m.group(1)) if m else cast(0)
        cat = re.search(r"Category::(\w+)", blk)
        out.append(dict(areaNo=f("areaNo", int), gridXNo=f("gridXNo", int),
                        gridZNo=f("gridZNo", int), posX=f("posX", float),
                        posZ=f("posZ", float), category=cat.group(1) if cat else "Unknown"))
    return out


def colour(cat):
    h = 0
    for ch in cat:
        h = (h * 31 + ord(ch)) & 0xFFFFFFFF
    return f"hsl({h % 360},70%,55%)"


def main():
    out_path = "render_map.html"
    area_filter = 60
    map_filter = None  # e.g. "m10_01" → render that one dungeon in its OWN local
                       # coords (its real interior layout, like a MapGenie submap),
                       # instead of projecting it onto the overworld.
    args = sys.argv[1:]
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a == "--area":
            area_filter = int(args[i + 1]); skip = True
        elif a == "--map":
            map_filter = args[i + 1]; skip = True
        elif not a.startswith("--"):
            out_path = a

    markers = parse_markers()
    pts, by_cat = [], defaultdict(int)
    skipped = 0
    for m in markers:
        if map_filter:
            # submap mode: raw local coords, no projection, only this map's rows
            if f"m{m['areaNo']:02d}_{m['gridXNo']:02d}" != map_filter:
                continue
            wx = m["gridXNo"] * 256.0 + m["posX"]
            wz = m["gridZNo"] * 256.0 + m["posZ"]
            pts.append((wx, wz, m["category"], m["areaNo"]))
            by_cat[m["category"]] += 1
            continue
        p = project(m["areaNo"], m["gridXNo"], m["gridZNo"], m["posX"], m["posZ"])
        if p is None:
            skipped += 1
            continue
        da, wx, wz = p
        if da != area_filter:
            continue
        pts.append((wx, wz, m["category"], m["areaNo"]))
        by_cat[m["category"]] += 1
    if not pts:
        print("no points for", map_filter or f"area {area_filter}")
        return
    label = map_filter if map_filter else f"area {area_filter}"

    xs = [p[0] for p in pts]; zs = [p[1] for p in pts]
    minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
    pad = 128
    W = (maxx - minx) + 2 * pad
    H = (maxz - minz) + 2 * pad
    # overworld: 0.5 px/unit (big, scrollable). submap: auto-fit a small dungeon
    # to ~900px so its interior layout is readable.
    SC = 0.5 if not map_filter else max(1.0, 900.0 / max(W, H, 1))

    def sx(wx): return (wx - minx + pad) * SC
    def sy(wz): return (H - (wz - minz + pad)) * SC

    # SVG circles grouped by class cat-<Category>
    circles = []
    for wx, wz, cat, src in pts:
        ring = ' stroke="#fff" stroke-width="0.4"' if src not in (60, 61) else ''
        circles.append(
            f'<circle class="cat-{cat}" cx="{sx(wx):.1f}" cy="{sy(wz):.1f}" r="2.6" '
            f'fill="{colour(cat)}"{ring}><title>{cat} (src m{src:02d})</title></circle>')
    svg = (f'<svg id="map" xmlns="http://www.w3.org/2000/svg" '
           f'width="{W*SC:.0f}" height="{H*SC:.0f}" viewBox="0 0 {W*SC:.0f} {H*SC:.0f}">'
           f'<rect width="100%" height="100%" fill="#0d0d0d"/>' + "".join(circles) + '</svg>')

    # sidebar grouped checkboxes
    bygrp = defaultdict(list)
    for cat, n in by_cat.items():
        bygrp[group_of(cat)].append((cat, n))
    rows = []
    for g in GROUPS + ["Other"]:
        if g not in bygrp:
            continue
        gtot = sum(n for _, n in bygrp[g])
        rows.append(f'<div class="grp"><label><input type="checkbox" checked '
                    f'onchange="grp(this,\'{g}\')"><b>{g}</b> ({gtot})</label></div>')
        for cat, n in sorted(bygrp[g], key=lambda kv: -kv[1]):
            rows.append(
                f'<label class="g-{g}"><input type="checkbox" checked data-cat="{cat}" '
                f'onchange="tog(this,\'{cat}\')">'
                f'<span class="sw" style="background:{colour(cat)}"></span>{cat} ({n})</label>')
    sidebar = "".join(rows)

    html = f"""<!doctype html><meta charset=utf-8><title>MapForGoblins render — {label}</title>
<style>
 body{{margin:0;font:12px system-ui;background:#0d0d0d;color:#ccc;display:flex}}
 #side{{width:280px;height:100vh;overflow:auto;padding:8px;box-sizing:border-box;background:#161616;flex:none}}
 #side h2{{font-size:14px;margin:4px 0}}
 #side label{{display:flex;align-items:center;gap:6px;padding:1px 0;cursor:pointer}}
 .grp{{margin-top:8px;border-top:1px solid #333;padding-top:4px}}
 .sw{{width:10px;height:10px;border-radius:50%;display:inline-block;flex:none}}
 #wrap{{flex:1;overflow:auto;height:100vh}}
 #map{{transform-origin:0 0}}
 .off{{display:none}}
 button{{background:#222;color:#ccc;border:1px solid #444;padding:3px 6px;cursor:pointer;margin:2px}}
</style>
<div id=side>
 <h2>{label} — {len(pts)} markers</h2>
 <div><button onclick="all(1)">all on</button><button onclick="all(0)">all off</button>
 <button onclick="zoom(1.25)">+</button><button onclick="zoom(0.8)">−</button></div>
 <div style="color:#888;font-size:11px">○ ringed = dungeon-projected. Scroll to pan, +/− to zoom.</div>
 {sidebar}
</div>
<div id=wrap>{svg}</div>
<script>
 var Z=1;
 // state-based: show/hide deterministically from the checkbox, never flip
 function tog(cb,c){{var hide=!cb.checked;document.querySelectorAll('.cat-'+CSS.escape(c)).forEach(function(e){{e.classList.toggle('off',hide)}})}}
 function grp(cb,g){{document.querySelectorAll('#side label.g-'+g+' input').forEach(function(i){{i.checked=cb.checked;tog(i,i.getAttribute('data-cat'))}})}}
 function all(on){{document.querySelectorAll('#side label[class^=g-] input').forEach(function(i){{i.checked=!!on;tog(i,i.getAttribute('data-cat'))}});document.querySelectorAll('.grp input').forEach(function(i){{i.checked=!!on}})}}
 function zoom(f){{Z*=f;document.getElementById('map').style.transform='scale('+Z+')'}}
 addEventListener('wheel',function(e){{if(e.ctrlKey){{e.preventDefault();zoom(e.deltaY<0?1.1:0.9)}}}},{{passive:false}});
</script>"""
    open(out_path, "w").write(html)
    print(f"wrote {out_path}: {len(pts)} markers, {len(by_cat)} categories "
          f"in {len([g for g in bygrp])} groups, {skipped} unmappable skipped")


if __name__ == "__main__":
    main()
