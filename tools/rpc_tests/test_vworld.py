#!/usr/bin/env python3
"""Virtual-world registry PERSISTENCE regression (World Virtualization vision #1, slice C3).

The custom-world registry saves to <mod>/virtual_worlds.toml and is boot-loaded (vworld::load_boot) at
dllmain. This proves the disk round-trip survives a REAL cold boot under Proton — the same TOML-load path
that was silently broken in the exceptions-ON toml++ config (see toml-parse-file-proton-bug.md). Without a
genuine reboot test the load-from-disk is never exercised (an in-session save+list would pass either way).

Two cold boots:
  boot 1: clear -> create world -> add 2 markers -> set active -> save -> assert listed with mk=2
  boot 2: (load_boot ran at dllmain) assert the world + its 2 markers + active id came back from disk
The pre-existing virtual_worlds.toml is backed up and restored so a dev's runtime is untouched.

Run: python tools/rpc_tests/test_vworld.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import GameSession  # noqa: E402

NAME = "MFGTestWorld"


def _winepath(p):
    """RPC reports a Wine path (e.g. Z:\\home\\…); map it back to the Linux host path (Z: -> /)."""
    p = p.replace("\\", "/")
    m = re.match(r"^[A-Za-z]:(/.*)$", p)
    return m.group(1) if m else p


def _mod_folder(g):
    """Derive the mod folder from `bundle status` (virtual_worlds.toml lives beside world_bundle.toml)."""
    reply = g.rpc("bundle status")
    m = re.search(r"path=(.+?)\s*$", reply)
    if not m:
        return None
    return os.path.dirname(_winepath(m.group(1).strip()))


def _world_id(reply):
    m = re.search(r"id=(\d+)", reply)
    return int(m.group(1)) if m else None


def main():
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""), flush=True)

    toml_path = None
    backup = None  # bytes of the pre-existing file, or None if there was none

    try:
        # --- boot 1: create + save ---
        with GameSession() as g:
            folder = _mod_folder(g)
            check("resolved mod folder", folder is not None and os.path.isdir(folder), str(folder))
            if folder:
                toml_path = os.path.join(folder, "virtual_worlds.toml")
                if os.path.exists(toml_path):
                    with open(toml_path, "rb") as f:
                        backup = f.read()

            g.rpc("vworld clear")
            wid = _world_id(g.rpc(f"vworld create {NAME}"))
            check("created world", wid is not None and wid >= 1, f"id={wid}")
            g.rpc(f"vworld marker {wid} 100 200 Alpha")
            g.rpc(f"vworld marker {wid} 300 400 Beta")
            active_reply = g.rpc(f"vworld active {wid}")
            check("set active", f"active={wid}" in active_reply, active_reply)
            save_reply = g.rpc("vworld save")
            check("saved", save_reply.startswith("ok vworld save"), save_reply)
            list1 = g.rpc("vworld list")
            check("boot1 list has world + mk=2",
                  f"[{wid}]{NAME}(mk=2)" in list1 and f"active={wid}" in list1, list1)

        check("virtual_worlds.toml exists on disk",
              toml_path is not None and os.path.exists(toml_path), toml_path or "no path")

        # --- boot 2: load_boot must restore it from disk ---
        with GameSession() as g:
            list2 = g.rpc("vworld list")
            check("boot2 reload: active id restored", f"active={wid}" in list2, list2)
            check("boot2 reload: world + 2 markers from disk",
                  f"[{wid}]{NAME}(mk=2)" in list2, list2)
    finally:
        # Restore the dev's pre-existing registry (or remove the test file) so runtime is untouched.
        if toml_path:
            try:
                if backup is not None:
                    with open(toml_path, "wb") as f:
                        f.write(backup)
                elif os.path.exists(toml_path):
                    os.remove(toml_path)
            except Exception as e:
                print(f"  [warn] restore virtual_worlds.toml failed: {e!r}", flush=True)

    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\n=== {passed}/{len(checks)} checks passed ===", flush=True)
    for n, ok, d in checks:
        if not ok:
            print(f"  FAIL: {n} — {d}", flush=True)
    sys.exit(0 if passed == len(checks) else 1)


if __name__ == "__main__":
    main()
