#!/usr/bin/env python3
"""Deploy the built MapForGoblins DLL into the ERR install's dll/offline — cross-platform.

The daily loop on Linux builds `build-linux/` and the VSCode "MFG: Deploy" task copies it in;
this is the SAME step for Windows, where the build dir is `build-err/` (build.bat / ninja) and the
game is launched by the user (not me3). After a deploy the game must be RESTARTED to load the new DLL
(a redeploy alone keeps the old one resident — see docs/memory/windows.md).

    python tools/deploy.py                 # copy DLL (+ PDB) from the OS-default build dir to $ERR_ROOT/dll/offline
    python tools/deploy.py --build-dir X   # override the source build dir
    python tools/deploy.py --no-pdb        # skip the (large) PDB

Paths resolve from .env.local (ERR_ROOT) via mfg_session — no hardcoding. Refuses to overwrite while
eldenring.exe is running (the loaded DLL is file-locked on Windows and stale-on-Linux anyway).
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from mfg_session import DLL_OFFLINE  # noqa: E402  (resolves ERR_ROOT via .env.local)


def _default_build_dir():
    # Windows = build.bat/ninja -> build-err/; Linux daily loop = build-linux/.
    return "build-err" if sys.platform == "win32" else "build-linux"


def _er_running():
    """True if eldenring.exe is live (deploy would fail / be pointless). Best-effort, cross-platform."""
    try:
        if sys.platform == "win32":
            out = os.popen('tasklist /FI "IMAGENAME eq eldenring.exe" /NH 2>NUL').read()
            return "eldenring.exe" in out.lower()
        out = os.popen("pgrep -x eldenring.exe 2>/dev/null || true").read()
        return bool(out.strip())
    except Exception:
        return False  # can't tell -> don't block


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default=os.environ.get("MFG_BUILD_DIR", _default_build_dir()),
                    help="source build directory (default: build-err on Windows, build-linux else)")
    ap.add_argument("--no-pdb", action="store_true", help="skip copying the PDB (26 MB)")
    ap.add_argument("--force", action="store_true",
                    help="deploy even if eldenring.exe appears to be running")
    a = ap.parse_args()

    repo = os.path.dirname(HERE)
    src_dir = a.build_dir if os.path.isabs(a.build_dir) else os.path.join(repo, a.build_dir)
    dll = os.path.join(src_dir, "MapForGoblins.dll")
    pdb = os.path.join(src_dir, "MapForGoblins.pdb")

    if not os.path.isfile(dll):
        sys.exit(f"deploy: no DLL at {dll} — build first (build.bat / ninja -C {a.build_dir} MapForGoblins).")
    if not os.path.isdir(DLL_OFFLINE):
        sys.exit(f"deploy: target {DLL_OFFLINE} does not exist — set ERR_ROOT in .env.local "
                 f"(see docs/memory/windows.md).")
    if _er_running() and not a.force:
        sys.exit("deploy: eldenring.exe is RUNNING — close the game first (the loaded DLL is locked; a "
                 "restart is required to load a new build anyway). Use --force to override.")

    shutil.copy2(dll, os.path.join(DLL_OFFLINE, "MapForGoblins.dll"))
    print(f"deployed MapForGoblins.dll -> {DLL_OFFLINE}")
    if not a.no_pdb and os.path.isfile(pdb):
        shutil.copy2(pdb, os.path.join(DLL_OFFLINE, "MapForGoblins.pdb"))
        print(f"deployed MapForGoblins.pdb -> {DLL_OFFLINE}")

    # Overlay translations: the DLL loads lang/<code>.txt from ITS OWN folder (goblin_i18n
    # g_mod_folder), NOT from the repo — without this sync the overlay silently falls back to
    # English (bit the Windows box 2026-07-07: dll/offline/lang/ never existed there, so the
    # whole F1 panel showed English and "Sauver dans l'INI" seemingly vanished).
    lang_src = os.path.join(repo, "assets", "lang")
    if os.path.isdir(lang_src):
        lang_dst = os.path.join(DLL_OFFLINE, "lang")
        os.makedirs(lang_dst, exist_ok=True)
        n = 0
        for f in os.listdir(lang_src):
            if f.endswith(".txt"):
                shutil.copy2(os.path.join(lang_src, f), os.path.join(lang_dst, f))
                n += 1
        print(f"deployed {n} lang file(s) -> {lang_dst}")
    print("-> RESTART the game to load the new DLL, then verify with `python tools/mfg.py rpc mfg_build`.")


if __name__ == "__main__":
    main()
