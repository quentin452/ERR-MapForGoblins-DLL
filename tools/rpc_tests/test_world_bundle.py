"""world_bundle disk-load regression — proves the TOML_EXCEPTIONS 0 migration (2026-07-04).

goblin_world_bundle records World-Editor edits and persists them to <mod>/world_bundle.toml, boot-loaded
(apply_boot) at dllmain. It USED to parse via the exceptions-ON ifstream+parse(string) path, which returns
an EMPTY table under Proton (silent, no throw) — so its on-disk load was latently broken. test_world_editor
never caught it: it saves+applies the IN-MEMORY bundle in one session and never cold-boot-reloads. This test
is the missing genuine save -> reboot -> load, asserting the recorded ops come back from disk.

Migration: goblin_world_bundle.cpp now uses `#define TOML_EXCEPTIONS 0` + toml::parse_file (the config
custom_items / virtual_world proved works under Proton). See toml-parse-file-proton-bug.md.

Two cold boots:
  boot 1: clear -> record 1 clone + 1 set -> save -> assert status clones=1 sets=1
  boot 2: (apply_boot ran at dllmain) assert `bundle status` shows clones=1 sets=1 (loaded FROM DISK)
The pre-existing world_bundle.toml is backed up and restored so a dev's runtime is untouched.

Run: python tools/rpc_tests/test_world_bundle.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import GameSession  # noqa: E402

PARAM = "ItemLotParam_map"
SRC = "900002000"     # an existing lot (same one test_world_editor repoints)
NEW = "900099001"     # arbitrary fresh row id — only the record round-trip is tested, not application
FIELD = "lotItemId02"
VALUE = "424242"


def _status(reply):
    """(clones, sets) from `ok clones=N sets=M path=…`, or (None, None)."""
    c = re.search(r"clones=(\d+)", reply)
    s = re.search(r"sets=(\d+)", reply)
    return (int(c.group(1)) if c else None, int(s.group(1)) if s else None)


def _winepath(p):
    """RPC reports a Wine path (e.g. Z:\\home\\…); map it back to the Linux host path (Z: -> /)."""
    p = p.replace("\\", "/")
    m = re.match(r"^[A-Za-z]:(/.*)$", p)
    return m.group(1) if m else p


def _toml_path(reply):
    m = re.search(r"path=(.+?)\s*$", reply)
    return _winepath(m.group(1).strip()) if m else None


def main():
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""), flush=True)

    toml_path = None
    backup = None

    try:
        # --- boot 1: record + save ---
        with GameSession() as g:
            toml_path = _toml_path(g.rpc("bundle status"))
            check("resolved bundle path", toml_path is not None, str(toml_path))
            if toml_path and os.path.exists(toml_path):
                with open(toml_path, "rb") as f:
                    backup = f.read()

            g.rpc("bundle clear")
            g.rpc(f"bundle clone {PARAM} {SRC} {NEW}")
            g.rpc(f"bundle set {PARAM} {NEW} {FIELD} {VALUE}")
            c, s = _status(g.rpc("bundle save"))
            check("boot1 saved clones=1 sets=1", c == 1 and s == 1, f"clones={c} sets={s}")

        check("world_bundle.toml exists on disk",
              toml_path is not None and os.path.exists(toml_path), toml_path or "no path")

        # --- boot 2: apply_boot must LOAD the ops from disk ---
        with GameSession() as g:
            c, s = _status(g.rpc("bundle status"))
            check("boot2 reload FROM DISK: clones=1 sets=1 (the TOML_EXCEPTIONS 0 fix)",
                  c == 1 and s == 1, f"clones={c} sets={s}")
    finally:
        if toml_path:
            try:
                if backup is not None:
                    with open(toml_path, "wb") as f:
                        f.write(backup)
                elif os.path.exists(toml_path):
                    os.remove(toml_path)
            except Exception as e:
                print(f"  [warn] restore world_bundle.toml failed: {e!r}", flush=True)

    passed = sum(1 for _, ok, _ in checks if ok)
    print(f"\n=== {passed}/{len(checks)} checks passed ===", flush=True)
    for n, ok, d in checks:
        if not ok:
            print(f"  FAIL: {n} — {d}", flush=True)
    sys.exit(0 if passed == len(checks) else 1)


if __name__ == "__main__":
    main()
