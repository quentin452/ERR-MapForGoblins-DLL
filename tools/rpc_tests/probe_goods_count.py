#!/usr/bin/env python3
"""Diagnostic (not pass/fail): map give_item -> goods_count response on one id.

Boots once, loads a save, then does a sequence of small grants/removes and prints the held
count after each, plus a node dump. Tells apart a goods_count read bug from give_item semantics
(add-only? qty ignored? stack cap at ~1000?). Run:
    python3 tools/rpc_tests/probe_goods_count.py [id] [id2 ...]
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from mfg_session import GameSession  # noqa: E402

IDS = sys.argv[1:] or ["0x40003bec"]


def n(reply):
    for tok in reply.split():
        if tok.startswith("n="):
            return tok.split("=", 1)[1]
    return "?"


def probe(g, idv):
    print(f"\n===== id {idv} =====", flush=True)
    print(f"  baseline           n={n(g.rpc(f'goods_count {idv}'))}", flush=True)
    for step in (1, 1, 1):
        g.rpc(f"give_item {idv} {step}")
        print(f"  after +{step}            n={n(g.rpc(f'goods_count {idv}'))}", flush=True)
    for step in (1, 1):
        g.rpc(f"give_item {idv} {-step}")
        print(f"  after -{step}            n={n(g.rpc(f'goods_count {idv}'))}", flush=True)
    # bigger explicit qty to see if it clamps to a stack cap
    g.rpc(f"give_item {idv} 5")
    print(f"  after +5            n={n(g.rpc(f'goods_count {idv}'))}", flush=True)


def main():
    with GameSession() as g:
        g.load_save()
        for idv in IDS:
            probe(g, idv)


if __name__ == "__main__":
    main()
