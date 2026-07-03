#!/usr/bin/env python3
"""Phase-2 clean-save regression (Variant A): a registered custom item must NOT survive in the
vanilla .sl2 once the .mfg no longer re-grants it.

Two cold boots (grants persist only through a real game save; the strip/reinject bracket fires on
the game's own save serialize, which `warp` triggers):
  boot 1: load -> grant reserved id (N x give_item +1) -> sidecar additem -> warp (game save;
          bracket strip@entry/reinject@exit runs) -> assert still held live -> kill
  between: empty the .mfg [items] on disk (so boot 2 does not re-grant it)
  boot 2: load -> assert goods_count == 0  (item was stripped from the .sl2, not re-granted)

Run: python3 tools/rpc_tests/test_custom_item.py
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import GameSession  # noqa: E402

TEST_ID = "0x40003bed"   # reserved goods 15341
QTY = 3

MFG_GLOB = os.path.expanduser(
    "~/.local/share/Steam/steamapps/compatdata/1245620/pfx/drive_c/users/steamuser/"
    "AppData/Roaming/EldenRing/*/ER0000.mfg")


def _n(reply):
    for tok in reply.split():
        if tok.startswith("n="):
            return int(tok.split("=", 1)[1])
    return None


def empty_mfg_items():
    """Blank the [items] section body in the .mfg on disk. Returns the path or None."""
    hits = glob.glob(MFG_GLOB)
    if not hits:
        return None
    path = hits[0]
    with open(path, "r", encoding="utf-8") as f:
        txt = f.read()
    # drop every row between the [items] header and the next [section] (or EOF)
    new = re.sub(r"(\[items\]\n)(?:(?!\[).*\n?)*", r"\1\n", txt)
    with open(path, "w", encoding="utf-8") as f:
        f.write(new)
    return path


checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""), flush=True)


def main():
    # --- boot 1: grant + register + game-save ---
    with GameSession() as g:
        g.load_save()
        base = _n(g.rpc(f"goods_count {TEST_ID}"))   # may be nonzero (a prior dirty save)
        for _ in range(QTY):
            g.rpc(f"give_item {TEST_ID} 1")
        held = _n(g.rpc(f"goods_count {TEST_ID}"))
        check(f"granted {QTY} live", held == base + QTY, f"base={base} held={held}")
        g.rpc(f"sidecar additem {TEST_ID} {QTY}")
        g.rpc("sidecar save")
        g.warp(11102950)                       # Roundtable Hold — triggers the game's save serialize
        still = _n(g.rpc(f"goods_count {TEST_ID}"))
        check("still held after save (restore)", still == base + QTY, f"held={still}")

    # --- between boots: empty the .mfg so boot 2 does not re-grant ---
    path = empty_mfg_items()
    check("emptied .mfg [items]", path is not None, path or "mfg not found")

    # --- boot 2: the item must be gone from the vanilla save ---
    with GameSession() as g:
        g.load_save()
        g.rpc("sidecar load")
        final = _n(g.rpc(f"goods_count {TEST_ID}"))
        check("clean save: goods_count == 0", final == 0, f"n={final}")

    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\n=== {passed}/{len(checks)} checks passed ===", flush=True)
    for n, ok, d in checks:
        if not ok:
            print(f"  FAIL: {n} — {d}", flush=True)
    sys.exit(0 if passed == len(checks) else 1)


if __name__ == "__main__":
    main()
