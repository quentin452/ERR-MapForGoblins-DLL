#!/usr/bin/env python3
"""Gap C grant vertical slice: a CLONED custom goods row can be granted into inventory AND kept
out of the vanilla .sl2 by the sidecar (define -> grant -> sidecar clean-save).

Distinct from test_custom_item.py (which grants a pre-EXISTING item id) — here the item id does
not exist until param_clone creates it this boot, proving the grant path works for genuinely
custom rows.

Key constraint (live-verified 2026-07-03): a goods id must be <= 0x7FFFFE (8388606) to be
grantable — AddItemFunc treats the goods id as a 23-bit field with 0x7FFFFF reserved, so the old
reserved band 90000001 was never grantable. RID below sits safely in-range and clear of real ER
goods ids (which cluster low).

Two cold boots (grants persist only through a real save; strip fires on the game's save serialize,
which warp triggers). NOTE param_clone does NOT persist (params reload from regulation each boot),
so boot 2 only checks the clean-save assertion, not re-definition. Run:
    python3 tools/rpc_tests/test_gapc_grant.py
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import GameSession, SAVE_DIR  # noqa: E402

RID = 8000000                       # reserved custom goods row id (<= 0x7FFFFE, grantable)
GID = f"0x{0x40000000 | RID:08x}"   # 0x407a1200 — category-encoded grant/count id
QTY = 1                             # template goods 100 is a maxNum=1 item (clone inherits the cap)

MFG_GLOB = os.path.join(SAVE_DIR, "*", "ER0000.mfg")


def _n(reply):
    for tok in reply.split():
        if tok.startswith("n="):
            return int(tok.split("=", 1)[1])
    return None


def empty_mfg_items():
    hits = glob.glob(MFG_GLOB)
    if not hits:
        return None
    with open(hits[0], "r", encoding="utf-8") as f:
        txt = f.read()
    with open(hits[0], "w", encoding="utf-8") as f:
        f.write(re.sub(r"(\[items\]\n)(?:(?!\[).*\n?)*", r"\1\n", txt))
    return hits[0]


def test(g):
    # --- boot 1: DEFINE (clone) -> GRANT -> sidecar register -> game-save ---
    g.load_save()
    g.assert_in(f"param_clone EquipParamGoods 100 {RID}", "new_row_present=1", "cloned custom goods row")
    base = _n(g.rpc(f"goods_count {GID}"))
    for _ in range(QTY):
        g.rpc(f"give_item {GID} 1")
    held = _n(g.rpc(f"goods_count {GID}"))
    g.check("cloned custom item granted into inventory", held == base + QTY, f"base={base} held={held}")
    g.rpc(f"sidecar additem {GID} {QTY}")
    g.rpc("sidecar save")
    g.warp(11102950)                       # Roundtable Hold — triggers the save serialize (strip)
    g.check("still held after save (restore)", _n(g.rpc(f"goods_count {GID}")) == base + QTY)

    # --- between boots: empty the .mfg so boot 2 does not re-grant ---
    g.check("emptied .mfg [items]", empty_mfg_items() is not None)


def test2(g):
    # --- boot 2: the custom item must be gone from the vanilla save ---
    g.load_save()
    g.rpc("sidecar load")
    g.check("clean save: custom item gone (goods_count == 0)", _n(g.rpc(f"goods_count {GID}")) == 0)


if __name__ == "__main__":
    # two isolated boots (boot-1 define/grant/save, boot-2 clean-save assert), both must pass
    ok = True
    with GameSession() as g:
        try:
            test(g)
        except Exception as e:
            g.check("boot1 raised", False, repr(e))
        ok &= g.summary()
    with GameSession() as g:
        try:
            test2(g)
        except Exception as e:
            g.check("boot2 raised", False, repr(e))
        ok &= g.summary()
    sys.exit(0 if ok else 1)
