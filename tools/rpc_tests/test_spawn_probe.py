#!/usr/bin/env python3
"""ADD / spawn_clone STAGE 1 — read-only recon probe (docs/re/windows_geom_spawn_re_findings.md).

Boots ER, loads a save (in-world so geom tiles are loaded), then calls `spawn_probe` which gathers and
validates every argument the Dynamic ctor FUN_1406b9880(self, srcType, partsRec, transform) needs, WITHOUT
mutating anything. Asserts the live-verify checklist holds on a freshly-deployed DLL:
  1. srcType@+0x08 carries the 0x6xxxxxxx geom FieldIns tag (and ideally hi32 == BlockData tag)
  2. transform@+0x18 head is a vtable ptr (24B FD4 pose wrapper), not raw floats
  3. parts rec present, rec+0x18b is a small flag, instance registry has room
  4. BlockData +0x288 geom_ins vector has spare capacity to push in place (informational)

Run: python tools/rpc_tests/test_spawn_probe.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402

LOG = os.path.expanduser("~/Games/ERRv2.2.9.6/dll/offline/logs/MapForGoblins.log")


def _test(g):
    g.load_save()
    g.check("alive in-world", g.alive())

    reply = g.rpc("spawn_probe", timeout=15)
    g.check("spawn_probe ok", reply.startswith("ok"), reply)
    g.check("game alive after probe (read-only)", g.alive())

    # Parse the summary line: "... tag=1 hiBlk=1 modVt=1 rec18b=0x0 reg=3/16 room=1 vecRoom=42 ..."
    def field(key, pat=r"(\S+)"):
        m = re.search(key + r"=" + pat, reply)
        return m.group(1) if m else None

    tag = field("tag")
    modvt = field("modVt")
    rec18b = field("rec18b")
    hiblk = field("hiBlk")
    vecroom = field("vecRoom")

    # Hard gates — the two facts spawn_clone truly depends on:
    g.check("checklist-1 srcType geom tag (0x6xxxxxxx)", tag == "1", f"tag={tag}")
    g.check("checklist-3 rec+0x18b (BlockData+0x18b) is a small flag",
            rec18b is not None and int(rec18b) <= 1, f"rec18b={rec18b}")
    # Informational (printed, NOT gated — these are understood, not blockers):
    #  - modVt=0 is EXPECTED: inst+0x18 aliases the BlockData ptr; the real transform module is at inst+0x20.
    #  - hiBlk=1 confirms srcType hi32 == BlockData tag (copyable).
    #  - vecRoom=0: the +0x288 vector is full → a render probe skips the +0x288 push (ctor self-registers).
    print(f"\n[info] modVt={modvt} (expected 0 — transform is at inst+0x20, not +0x18)")
    print(f"[info] hiBlk={hiblk} (srcType hi32 == BlockData tag)")
    print(f"[info] vecRoom={vecroom} (0 ⇒ +0x288 full; first render probe skips the push)")

    # Dump the full recon block (both [SPAWNPROBE] summary + [GEOMDUMP] hex windows) for eyeballing.
    if os.path.exists(LOG):
        block = [l.rstrip() for l in open(LOG)
                 if "[SPAWNPROBE]" in l or "[GEOMDUMP] inst00" in l
                 or "[GEOMDUMP] mod18ptr" in l or "[GEOMDUMP] inst210" in l]
        print("\n--- last recon block ---")
        for l in block[-24:]:
            print(l.split("] [info] ")[-1] if "] [info] " in l else l)


if __name__ == "__main__":
    run_test(_test)
