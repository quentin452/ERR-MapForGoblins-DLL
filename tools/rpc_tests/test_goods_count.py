#!/usr/bin/env python3
"""Regression: goblin::inventory::goods_count reads the live held qty (offset verify).

Reads a fresh test id's held count, grants two single units, and asserts the count tracks each
grant by exactly +1 — i.e. the Ghidra-pinned EquipInventoryData walk in
docs/re/windows_goods_count_re_findings.md reads the real quantity in-world.

Uses give_item(+1) repeated (the verified exact grant primitive). NOTE give_item is ADD-ONLY:
negative qty is a no-op and qty>=~5 clamps to the item's stack cap (~1000) — so this test never
relies on removal or multi-qty grants. Cleanup is not attempted (the game has no remove path here);
the fresh id starts at 0 on a clean save. Run:
    python3 tools/rpc_tests/test_goods_count.py
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import run_test  # noqa: E402

TEST_ID = "0x40003bed"   # 0x40000000 | goods 15341 — reserved test id (0 held on a clean save)
CAP = 990                # give_item clamps near the stack cap; skip the increment asserts above this


def _n(reply):
    for tok in reply.split():
        if tok.startswith("n="):
            return int(tok.split("=", 1)[1])
    return None


def test(g):
    g.load_save()

    base_r = g.rpc(f"goods_count {TEST_ID}")
    g.check("goods_count reads in-world", base_r.startswith("ok"), base_r)
    if not base_r.startswith("ok"):
        return
    base = _n(base_r)
    g.check("baseline is a real count", base is not None, base_r)

    if base >= CAP:
        g.check("near stack cap — skip increment asserts", True, f"base={base}")
        return

    g.assert_in(f"give_item {TEST_ID} 1", "ok", "give_item +1 ok")
    a1 = _n(g.rpc(f"goods_count {TEST_ID}"))
    g.check("count == base+1 after first grant", a1 == base + 1, f"base={base} a1={a1}")

    g.assert_in(f"give_item {TEST_ID} 1", "ok", "give_item +1 ok (2nd)")
    a2 = _n(g.rpc(f"goods_count {TEST_ID}"))
    g.check("count == base+2 after second grant", a2 == base + 2, f"base={base} a2={a2}")

    # Record the EquipInventoryData segment header for the log.
    g.rpc("equip_dump 0x158 0x90")


SWEEP = test  # run_all.py aggregation entry — single-boot, self-loads (safe to share a session)


if __name__ == "__main__":
    run_test(test)
