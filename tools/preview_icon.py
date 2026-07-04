#!/usr/bin/env python3
"""Preview a native map-point icon BY NAME — "decompile the texture from its name".

The runtime parses the name->(sheet,x,y,w,h) rect table from the active install's sblytbnd
(Scaleform layout binder); there is no committed rect file. So this asks the RUNNING game for
the rect (RPC `map_rect <name>`), then crops that sub-rect from the extracted sheet PNG in
tools/extracted/ and saves an upscaled preview. One or many names -> a labelled montage.

Prereq: ER running with a current DLL deployed (Steam up), be the SOLE RPC client (the debug
listener serves one connection — close any repl/GameSession socket first), and the layout parsed
(the DLL parses it once the native map has been opened; if a name reports found=0, open the
map in-game once). Sheets live in tools/extracted/ (e.g. SB_MapCursor.png, SB_MapCursor_02.png)
— extracted by tools/build_menu_tex_extract.sh.

Usage:
  python tools/preview_icon.py MENU_MAP_Player_01
  python tools/preview_icon.py MENU_MAP_Player_01 MENU_MAP_Bearing MENU_MAP_Cursor_01
  python tools/preview_icon.py Player_01 Bearing          # MENU_MAP_ prefix optional
  python tools/preview_icon.py --port 38700 -o /tmp/out.png Player_01
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mfg_rpc import Rpc  # noqa: E402

EXTRACTED = os.path.join(HERE, "extracted")
DEFAULT_PORT = int(os.environ.get("MFG_RPC_PORT", "38700"))
RECT_RE = re.compile(r"sheet=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) found=(\d)")


def fetch_rect(rpc, name):
    """RPC map_rect -> (sheet, x, y, w, h) or None. Prepends MENU_MAP_ if the caller omitted it."""
    full = name if name.startswith("MENU_MAP_") else "MENU_MAP_" + name
    reply = rpc.cmd("map_rect " + full)
    m = RECT_RE.search(reply)
    if not m or m.group(6) != "1":
        print(f"  {full}: not found ({reply.strip()})  — open the native map in-game once so the layout parses")
        return None
    return (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))


def main():
    ap = argparse.ArgumentParser(description="Preview native map-point icons by name (crop from the sheet).")
    ap.add_argument("names", nargs="+", help="MENU_MAP_* icon names (the MENU_MAP_ prefix is optional)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help="debug RPC port (default 38700)")
    ap.add_argument("-o", "--out", default="/tmp/preview_icon.png", help="output PNG path")
    ap.add_argument("--cell", type=int, default=150, help="montage cell size px")
    a = ap.parse_args()

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        sys.exit("preview_icon: needs Pillow (pip install pillow)")

    try:
        rpc = Rpc(a.port)
    except (OSError, ConnectionError):
        sys.exit(f"preview_icon: no RPC listener on 127.0.0.1:{a.port} — is ER running with the DLL deployed?")

    sheets = {}
    def sheet_img(name):
        if name not in sheets:
            path = os.path.join(EXTRACTED, name)
            if not os.path.exists(path):
                raise FileNotFoundError(f"sheet not extracted: {path} (run tools/build_menu_tex_extract.sh)")
            sheets[name] = Image.open(path).convert("RGBA")
        return sheets[name]

    crops = []
    try:
        for nm in a.names:
            rect = fetch_rect(rpc, nm)
            if not rect:
                continue
            sheet, x, y, w, h = rect
            try:
                im = sheet_img(sheet).crop((x, y, x + w, y + h))
            except FileNotFoundError as e:
                print(f"  {nm}: {e}")
                continue
            crops.append((nm.replace("MENU_MAP_", ""), sheet, im))
            print(f"  {nm}: {sheet} [{x},{y} {w}x{h}]")
    finally:
        rpc.close()

    if not crops:
        sys.exit("preview_icon: nothing to preview")

    cell, pad, cols = a.cell, 26, min(4, len(crops))
    rows = (len(crops) + cols - 1) // cols
    mont = Image.new("RGBA", (cols * cell, rows * (cell + pad)), (40, 40, 48, 255))
    d = ImageDraw.Draw(mont)
    for i, (label, sheet, im) in enumerate(crops):
        sc = min((cell - 8) / im.width, (cell - 8) / im.height)
        im2 = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))))
        cx, cy = (i % cols) * cell, (i // cols) * (cell + pad)
        d.rectangle([cx, cy, cx + cell, cy + cell], fill=(70, 70, 80, 255))  # bg to show alpha
        mont.alpha_composite(im2, (cx + (cell - im2.width) // 2, cy + (cell - im2.height) // 2))
        d.text((cx + 2, cy + cell + 4), label, fill=(230, 230, 240, 255))
    mont.convert("RGB").save(a.out)
    print(f"wrote {a.out}  ({len(crops)} icon(s))")


if __name__ == "__main__":
    main()
