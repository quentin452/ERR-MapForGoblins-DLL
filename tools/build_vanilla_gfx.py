#!/usr/bin/env python3
"""Build the non-ERR worldmap GFX (vanilla or convergence base) + our icons.

The shipped ERR gfx (assets/menu/02_120_worldmap_new.gfx) is built on top of
ERR's modified 02_120_worldmap.gfx, which re-icons several existing frames
(boss/camp/bounty/grace/remembrance/tower) with ERR-only textures
(MENU_MAP_ERR_*, charIds 13500-13506) that don't exist in vanilla 01_common.
Shipping it to other players would carry ERR's UI changes (and dead texture
references). This tool rebuilds the icon set on the target game's own base:

  vanilla     base: GAME_DIR/menu/02_120_worldmap.gfx (sprite 171 = 348 frames)
              out:  assets/menu/02_120_worldmap_vanilla.gfx
  convergence base: CONVERGENCE_MOD_DIR/menu/02_120_worldmap.gfx (756 frames —
              the mod adds 408 icon frames of its own, so OUR frames land at
              757+ and the bake shifts iconIds by config.ICON_FRAME_OFFSET;
              this tool verifies that constant against the actual base)
              out:  assets/menu/02_120_worldmap_convergence.gfx

  1. decompile the base gfx and our _new.gfx (FFDEC swf2xml),
  2. copy our added DefineExternalImage2 defs (charIds 1000-1024 — all
     vanilla texture names: MENU_Tab_*, MENU_ItemIcon_*, ...) into the
     base XML before sprite 171 (verifying the base doesn't already use
     those charIds),
  3. append our added frames (349..end of OUR gfx; every frame is a
     self-contained RemoveObject2+PlaceObject3+ShowFrame group) to the base
     sprite 171 and bump its frameCount,
  4. compile back (FFDEC xml2swf).

Verifies that no appended frame references an ERR-only charId.

FFDEC: set the FFDEC_CLI env var (e.g. 'java -jar C:/.../ffdec-cli.jar'),
otherwise the known local install next to the project is used.

Usage: py build_vanilla_gfx.py [--profile vanilla|convergence]
       (default: the active MFG_PROFILE if non-ERR, else vanilla)
"""
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import config

PROJECT = config.PROJECT_DIR
OURS_GFX = PROJECT / "assets" / "menu" / "02_120_worldmap_new.gfx"

VANILLA_SPRITE_FRAMES = 348          # frames in vanilla sprite 171
OURS_FIRST_FRAME = 349               # our frames start here in OUR gfx
OUR_IMAGE_IDS = set(range(1000, 1025))   # DefineExternalImage2 we added
ERR_IMAGE_IDS = set(range(13500, 13507))  # ERR-only textures — must NOT leak

# Per-profile expected base frame count. Must satisfy:
#   base_frames == VANILLA_SPRITE_FRAMES + config-side ICON_FRAME_OFFSET
# (the bake shifts our iconIds by that offset; see config.py). If a target
# mod update changes its frame count, update BOTH places.
PROFILE_BASE_FRAMES = {"vanilla": 348, "convergence": 756}

# ── Cleared-marker badge (boss/invader defeated overlay) ──
# The worldmap marker (sprite 174) draws the icon (sprite 171) + a "cleared"
# badge (sprite 173 -> shape 172). Vanilla's native badge is a small gold dot
# placed far up-right of the icon (174->173 translate (566,-448)); ERR reskins
# it to a green checkmark that overlaps the icon (effective centre ≈ (-335,-336)
# twips). For our non-ERR builds we (a) replace shape 172 with our own badge
# and (b) move the badge to the ERR placement so it overlaps the icon.
#
# IMPORTANT — the badge is an EMBEDDED RASTER, not a vector. FFDEC can't import
# detailed SVGs as Scaleform shapes (clip-paths/gradients garble; and a shape
# crashes the map once it exceeds ~30-45 filled figures). So make_cleared_badge.py
# produces a small PNG (assets/badges/cleared_badge.png) from the source art
# (assets/badges/mark2.png) and `ffdec -replace ... 172 <png>` embeds it as a
# DefineBitsLossless2 bitmap-fill shape (1 figure, centred ±257 = 26px, renders
# fine in-game — verified). Vanilla-only for now (Convergence ships its own
# MENU_MAP_Boss_Dead badge via sprite 173 -> char 902, left untouched).
BADGE_PROFILES = {"vanilla"}
BADGE_IMG = PROJECT / "assets" / "badges" / "cleared_badge.png"
BADGE_SPRITE_PARENT = 174     # marker container that places the badge sprite
BADGE_SPRITE_ID = 173         # the badge sprite
BADGE_SHAPE_ID = 172          # the shape the badge sprite draws
BADGE_POS_TWIPS = (-335, -336)  # ERR-matched centre (overlaps the icon)


def ffdec_cmd():
    env = os.environ.get("FFDEC_CLI")
    if env:
        return env.split()
    java = Path("E:/Program Files/Java/jdk1.8.0_211/bin/java.exe")
    jar = PROJECT.parent / "FFDec - Zipped" / "ffdec-cli.jar"
    if java.exists() and jar.exists():
        return [str(java), "-jar", str(jar)]
    print("ERROR: set FFDEC_CLI (e.g. 'java -jar /path/to/ffdec-cli.jar')")
    sys.exit(1)


def run_ffdec(args):
    r = subprocess.run(ffdec_cmd() + args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:], r.stderr[-2000:])
        sys.exit(f"FFDEC failed: {' '.join(args)}")


def sprite171(root):
    for it in root.iter("item"):
        if it.get("type") == "DefineSpriteTag" and it.get("spriteId") == "171":
            return it
    sys.exit("sprite 171 not found")


def main():
    profile = None
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == "--profile" and i + 1 < len(args):
            profile = args[i + 1].strip().lower()
        elif a.startswith("--profile="):
            profile = a.split("=", 1)[1].strip().lower()
    if profile is None:
        profile = config.PROFILE if config.PROFILE != "err" else "vanilla"
    if profile not in PROFILE_BASE_FRAMES:
        sys.exit(f"unknown profile '{profile}' (expected: {sorted(PROFILE_BASE_FRAMES)})")

    if profile == "convergence":
        if not config.CONVERGENCE_MOD_DIR:
            sys.exit("set convergence_mod_dir in tools/config.ini")
        base_gfx = config.CONVERGENCE_MOD_DIR / "menu" / "02_120_worldmap.gfx"
    else:
        base_gfx = config.require_game_dir() / "menu" / "02_120_worldmap.gfx"
    if not base_gfx.exists():
        sys.exit(f"base gfx not found: {base_gfx}")
    base_frames = PROFILE_BASE_FRAMES[profile]
    out_gfx = PROJECT / "assets" / "menu" / f"02_120_worldmap_{profile}.gfx"
    print(f"profile={profile}  base={base_gfx}")

    tmp = Path(tempfile.mkdtemp(prefix="mfg_gfx_"))
    try:
        van_xml, our_xml, out_xml = tmp / "van.xml", tmp / "our.xml", tmp / "out.xml"
        print("decompiling base + ours...")
        run_ffdec(["-swf2xml", str(base_gfx), str(van_xml)])
        run_ffdec(["-swf2xml", str(OURS_GFX), str(our_xml)])

        van = ET.parse(van_xml)
        our = ET.parse(our_xml)
        vroot, oroot = van.getroot(), our.getroot()

        # ── our image defs (charIds 1000-1024) ──
        our_images = []
        for it in oroot.iter("item"):
            if it.get("type") == "DefineExternalImage2":
                cid = int(it.get("characterID") or 0)
                if cid in OUR_IMAGE_IDS:
                    our_images.append(it)
        if len(our_images) != len(OUR_IMAGE_IDS):
            sys.exit(f"expected {len(OUR_IMAGE_IDS)} image defs, found {len(our_images)}")

        # ── our sprite-171 frames after the vanilla range (in OUR gfx our
        # frames always start at 349 — the ERR base adds no frames) ──
        osub = sprite171(oroot).find("subTags")
        frame = 0
        added = []
        for child in list(osub):
            if frame >= VANILLA_SPRITE_FRAMES and child.get("type") != "EndTag":
                added.append(child)
            if child.get("type") == "ShowFrameTag":
                frame += 1
        n_added = sum(1 for c in added if c.get("type") == "ShowFrameTag")
        print(f"ours: {frame} frames, appending {n_added} "
              f"(land at {base_frames + 1}..{base_frames + n_added} on this base)")

        # safety: no ERR-only textures in what we append
        for c in added:
            for el in [c] + list(c.iter()):
                cid = el.get("characterId")
                if cid and int(cid) in ERR_IMAGE_IDS:
                    sys.exit(f"appended frame references ERR-only charId {cid}")

        # ── merge into the base ──
        vsprite = sprite171(vroot)
        vsub = vsprite.find("subTags")
        if int(vsprite.get("frameCount")) != base_frames:
            sys.exit(f"base sprite 171 has {vsprite.get('frameCount')} frames, "
                     f"expected {base_frames} — base mod/game updated? Update "
                     f"PROFILE_BASE_FRAMES here AND ICON_FRAME_OFFSET in config.py")

        # the base must not already define our charId range (we'd collide)
        for it in vroot.iter("item"):
            if it.get("type") == "DefineExternalImage2":
                cid = int(it.get("characterID") or 0)
                if cid in OUR_IMAGE_IDS:
                    sys.exit(f"base gfx already uses charId {cid} (our range "
                             f"1000-1024) — pick a new range before merging")

        # insert image defs right before the sprite 171 definition (top level)
        # find the top-level container holding the sprite
        parent = None
        for cand in vroot.iter():
            if vsprite in list(cand):
                parent = cand
                break
        idx = list(parent).index(vsprite)
        for img in reversed(our_images):
            parent.insert(idx, img)

        # drop the EndTag, append frames, restore EndTag
        end_tags = [c for c in list(vsub) if c.get("type") == "EndTag"]
        for e in end_tags:
            vsub.remove(e)
        for c in added:
            vsub.append(c)
        for e in end_tags:
            vsub.append(e)
        vsprite.set("frameCount", str(base_frames + n_added))

        # ── reposition the cleared badge to overlap the icon (badge profiles) ──
        if profile in BADGE_PROFILES:
            moved = False
            for sp in vroot.iter("item"):
                if sp.get("type") == "DefineSpriteTag" and sp.get("spriteId") == str(BADGE_SPRITE_PARENT):
                    for ch in sp.iter("item"):
                        if (ch.get("type") in ("PlaceObject2Tag", "PlaceObject3Tag")
                                and ch.get("characterId") == str(BADGE_SPRITE_ID)):
                            mtx = ch.find(".//matrix")
                            if mtx is not None:
                                mtx.set("hasTranslate", "true")
                                mtx.set("translateX", str(BADGE_POS_TWIPS[0]))
                                mtx.set("translateY", str(BADGE_POS_TWIPS[1]))
                                moved = True
            if not moved:
                sys.exit(f"badge: could not find sprite {BADGE_SPRITE_PARENT}->{BADGE_SPRITE_ID} placement")
            print(f"badge: moved {BADGE_SPRITE_PARENT}->{BADGE_SPRITE_ID} to {BADGE_POS_TWIPS} twips")

        van.write(out_xml, encoding="utf-8", xml_declaration=True)
        print("compiling...")
        run_ffdec(["-xml2swf", str(out_xml), str(out_gfx)])

        # ── embed our badge raster into the badge shape (badge profiles) ──
        if profile in BADGE_PROFILES:
            if not BADGE_IMG.exists():
                sys.exit(f"badge png missing: {BADGE_IMG} (run tools/make_cleared_badge.py)")
            run_ffdec(["-replace", str(out_gfx), str(out_gfx),
                       str(BADGE_SHAPE_ID), str(BADGE_IMG)])
            print(f"badge: embedded {BADGE_IMG.name} into shape {BADGE_SHAPE_ID}")

        print(f"wrote {out_gfx} ({out_gfx.stat().st_size} bytes, "
              f"sprite 171: {base_frames + n_added} frames)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
