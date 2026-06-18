#!/usr/bin/env python3
"""Draw the cluster map-icon overlay ("many markers piled here").

Output: assets/badges/cluster_glyph.png (160x160 RGBA, centered, transparent pad).

Clustering (Thread 5) collapses a dense pile of markers into one synthetic row.
That row used to borrow the anonymous gray "?" frame, which reads as "unknown"
rather than "a stack of N markers". This glyph gives clusters their own look:
three overlapping filled discs (a "cluster of dots") in a saturated teal with a
dark outline — visually distinct from the "?" (gray) and from grace/loot/hostile,
and clusters are never themselves clustered so it never needs to nest.

160 matches the vanilla item-overlay texture size (MENU_ItemIcon_* = 160), so
build_vanilla_gfx embeds it into the cloned shape and places it at the standard
overlay scale — exactly like the "?" frame (see make_anon_icon.py)."""
from pathlib import Path
import config
from PIL import Image, ImageDraw

DST = config.PROJECT_DIR / "assets" / "badges" / "cluster_glyph.png"
SIZE = 160
SS = 4                          # supersample factor for clean antialiased edges
PAD = 0.92                      # glyph fills 92% of the tile

# Distinct from: "?" (gray), grace (gold), loot, hostile (red).
FILL    = (43, 183, 196, 255)   # saturated teal
OUTLINE = (16, 38, 46, 255)     # dark navy edge
RING    = (235, 250, 252, 255)  # light inner ring for contrast on the map


def main():
    n = SIZE * SS
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    span = n * PAD                       # bounding span the cluster occupies
    r = span * 0.34                      # disc radius
    cx = n / 2
    # three discs: one top-centre, two on the bottom row — a triangular cluster
    off = span * 0.30
    centres = [(cx, cx - off * 1.05),    # top
               (cx - off, cx + off * 0.55),  # bottom-left
               (cx + off, cx + off * 0.55)]  # bottom-right
    ow = max(2, int(r * 0.16))           # outline width
    rw = max(1, int(r * 0.10))           # inner ring width

    # back-to-front so overlaps read cleanly; bottom row first, top last
    for x, y in (centres[1], centres[2], centres[0]):
        d.ellipse([x - r, y - r, x + r, y + r], fill=FILL, outline=OUTLINE, width=ow)
    # inner highlight ring on each disc, drawn after fills so it stays visible
    ir = r * 0.62
    for x, y in (centres[1], centres[2], centres[0]):
        d.ellipse([x - ir, y - ir, x + ir, y + ir], outline=RING, width=rw)

    im = im.resize((SIZE, SIZE), Image.LANCZOS)
    DST.parent.mkdir(parents=True, exist_ok=True)
    im.save(DST)
    print(f"wrote {DST} ({im.size})")


if __name__ == "__main__":
    main()
