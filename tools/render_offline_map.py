#!/usr/bin/env python3
"""Offline full-map render of every MapForGoblins marker — no game, no fog, no save risk.

Projects each baked marker onto the overworld plane with the SAME logic the DLL
uses at inject (LEGACY_CONV exact match, then same-src_area fallback), then emits
a zoomable SVG: one dot per marker, coloured by category. Open in a browser and
zoom freely to inspect placement / coverage / dungeon clusters without the
in-game fog of war.

Usage:  tools/render_offline_map.py [out.svg] [--area 60|61]
        (default: render_map.svg, area 60 = Lands Between overworld)
"""
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "src", "generated")


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
    """Replicate the DLL: overworld passthrough, else exact (area,gx) then same-area fallback.
    Returns (dst_area, world_x, world_z) or None if unmappable."""
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
                        posZ=f("posZ", float), category=cat.group(1) if cat else "?"))
    return out


# Stable colour per category (hash → HSL hue). Distinct enough for a legend.
def colour(cat):
    h = 0
    for ch in cat:
        h = (h * 31 + ord(ch)) & 0xFFFFFFFF
    return f"hsl({h % 360},70%,50%)"


def main():
    out_path = "render_map.svg"
    area_filter = 60
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--area":
            area_filter = int(args[i + 1])
        elif not a.startswith("--") and (i == 0 or args[i - 1] != "--area"):
            out_path = a

    markers = parse_markers()
    pts = []
    by_cat = defaultdict(int)
    skipped = 0
    for m in markers:
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
        print("no points for area", area_filter)
        return
    xs = [p[0] for p in pts]
    zs = [p[1] for p in pts]
    minx, maxx, minz, maxz = min(xs), max(xs), min(zs), max(zs)
    pad = 128
    W = (maxx - minx) + 2 * pad
    H = (maxz - minz) + 2 * pad
    SC = 0.5  # px per world unit

    def sx(wx):
        return (wx - minx + pad) * SC

    def sy(wz):
        return (H - (wz - minz + pad)) * SC  # flip Z for screen-down Y

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W*SC:.0f}" height="{H*SC:.0f}" '
           f'viewBox="0 0 {W*SC:.0f} {H*SC:.0f}">',
           f'<rect width="100%" height="100%" fill="#111"/>',
           f'<text x="10" y="20" fill="#ccc" font-size="14">MapForGoblins offline render — '
           f'area {area_filter} — {len(pts)} markers</text>']
    for wx, wz, cat, srcarea in pts:
        # dungeon-projected markers (srcarea != 60/61) ringed so they stand out
        ring = ' stroke="#fff" stroke-width="0.4"' if srcarea not in (60, 61) else ''
        svg.append(f'<circle cx="{sx(wx):.1f}" cy="{sy(wz):.1f}" r="1.6" '
                   f'fill="{colour(cat)}"{ring}><title>{cat} (src m{srcarea:02d})</title></circle>')
    # legend
    ly = 40
    for cat, n in sorted(by_cat.items(), key=lambda kv: -kv[1]):
        svg.append(f'<circle cx="16" cy="{ly}" r="4" fill="{colour(cat)}"/>'
                   f'<text x="26" y="{ly+4}" fill="#ccc" font-size="11">{cat} ({n})</text>')
        ly += 15
    svg.append('</svg>')
    open(out_path, "w").write("\n".join(svg))
    print(f"wrote {out_path}: {len(pts)} markers on area {area_filter}, "
          f"{len(by_cat)} categories, {skipped} unmappable skipped")
    print(f"world bounds x[{minx:.0f},{maxx:.0f}] z[{minz:.0f},{maxz:.0f}]")


if __name__ == "__main__":
    main()
