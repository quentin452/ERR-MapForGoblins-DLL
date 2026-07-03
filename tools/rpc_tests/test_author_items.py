#!/usr/bin/env python3
"""Regression: the custom_items.toml author surface defines + grants a custom item end-to-end.

Writes a custom_items.toml into the mod folder, cold-boots (the applier runs at init: clone template
row -> set fields -> inject name -> register with the sidecar), loads a save (world-enter reinject
grants it), and asserts goods_count == qty. Restores any pre-existing toml on exit.

Run: python3 tools/rpc_tests/test_author_items.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import run_test, DLL_OFFLINE  # noqa: E402

MOD_DIR = DLL_OFFLINE
TOML = os.path.join(MOD_DIR, "custom_items.toml")
RID = 8000001                       # fresh reserved goods row (<= 0x7FFFFE)
GID = f"0x{0x40000000 | RID:08x}"   # 0x407a1201
QTY = 1

CONTENT = f"""# author-surface regression (test_author_items.py)
[[goods]]
id     = {RID}
clone  = 100
name   = "Author Regression Item"
qty    = {QTY}
fields = {{ sortGroupId = 101 }}
"""


def _n(reply):
    for tok in reply.split():
        if tok.startswith("n="):
            return int(tok.split("=", 1)[1])
    return None


def test(g):
    g.load_save()
    r = g.rpc(f"goods_count {GID}")
    g.check("author item granted from toml", _n(r) == QTY, r)


if __name__ == "__main__":
    # stage the toml (back up any existing one), run one boot, restore.
    backup = None
    if os.path.exists(TOML):
        backup = TOML + ".testbak"
        os.replace(TOML, backup)
    with open(TOML, "w", encoding="utf-8") as f:
        f.write(CONTENT)
    try:
        run_test(test)   # calls sys.exit()
    finally:
        os.remove(TOML) if os.path.exists(TOML) else None
        if backup:
            os.replace(backup, TOML)
