#!/usr/bin/env python3
"""Draw the "cluster depleted/done" map-icon overlay (all members collected).

Output: assets/badges/cluster_done_glyph.png (160x160 RGBA, centered, transparent pad).

The green sibling of the teal cluster glyph (make_cluster_icon.py). Same "stack of
dots" silhouette so it reads as the same family, but GREEN = "cleared/done": the DLL
swaps a cluster's iconId to CLUSTER_DONE_ICON_ID once every member is collected, so
the pile reads as depleted instead of showing a stale count. Distinct from teal
cluster / blue quest-NPC / gold grace / red hostile. 160 matches the vanilla overlay
size so build_vanilla_gfx embeds it into the cloned shape (same trick as the others)."""
from pathlib import Path
import config
from PIL import Image, ImageDraw

DST = config.PROJECT_DIR / "assets" / "badges" / "cluster_done_glyph.png"
SIZE = 160
SS = 4                          # supersample factor for clean antialiased edges
PAD = 0.92                      # glyph fills 92% of the tile

# Green "cleared/done" — distinct from teal cluster, blue quest-NPC, gold grace, red hostile.
FILL    = (60, 190, 95, 255)    # saturated green
OUTLINE = (16, 46, 24, 255)     # dark green edge
RING    = (236, 252, 240, 255)  # light inner ring for contrast on the map


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
