#!/usr/bin/env python3
"""Draw the quest-NPC map-icon overlay (named friendly NPCs + merchants).

Output: assets/badges/quest_npc_glyph.png (160x160 RGBA, centered, transparent pad).

The friendly sibling of the hostile-NPC icon (374). A person bust (head +
shoulders) silhouette in a friendly blue with a dark outline — reads as "an NPC
is here / someone to talk to", visually distinct from grace (gold), loot,
hostile (red), the cluster glyph (teal), and the gray "?". 160 matches the
vanilla item-overlay texture size so build_vanilla_gfx embeds it into the cloned
shape and places it at the standard overlay scale (same trick as the "?" and
cluster frames — see make_anon_icon.py / make_cluster_icon.py)."""
from pathlib import Path
import config
from PIL import Image, ImageDraw

DST = config.PROJECT_DIR / "assets" / "badges" / "quest_npc_glyph.png"
SIZE = 160
SS = 4                          # supersample for clean antialiased edges
PAD = 0.90                      # glyph fills 90% of the tile

# Distinct from: "?" (gray), grace (gold), hostile (red), cluster (teal),
# material nodes (green).
FILL    = (62, 143, 208, 255)   # friendly blue
OUTLINE = (16, 34, 54, 255)     # dark navy edge
HILITE  = (224, 240, 252, 255)  # light rim for contrast on the map


def main():
    n = SIZE * SS
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    span = n * PAD
    cx = n / 2
    ow = max(2, int(span * 0.045))     # outline width

    # head: circle in the upper third
    hr = span * 0.20                   # head radius
    hy = cx - span * 0.20              # head centre y
    # shoulders: a wide rounded "bust" arc below the head
    bw = span * 0.62                   # shoulder half-width
    bt = hy + hr * 0.55                # shoulders top
    bb = cx + span * 0.42              # shoulders bottom (clipped into a dome)

    # draw shoulders first (a rounded-top dome via pieslice), then head on top
    d.pieslice([cx - bw, bt, cx + bw, bt + 2 * (bb - bt)],
               180, 360, fill=FILL, outline=OUTLINE, width=ow)
    d.ellipse([cx - hr, hy - hr, cx + hr, hy + hr],
              fill=FILL, outline=OUTLINE, width=ow)
    # subtle highlight rim on the head
    rw = max(1, int(hr * 0.10))
    ir = hr * 0.66
    d.ellipse([cx - ir, hy - ir, cx + ir, hy + ir], outline=HILITE, width=rw)

    im = im.resize((SIZE, SIZE), Image.LANCZOS)
    DST.parent.mkdir(parents=True, exist_ok=True)
    im.save(DST)
    print(f"wrote {DST} ({im.size})")


if __name__ == "__main__":
    main()
