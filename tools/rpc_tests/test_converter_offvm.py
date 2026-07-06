#!/usr/bin/env python3
"""OFF-VM converter validation (fd0ad45 "Remaining" step) — prove the world->map-space affine is
reproducible with the native map NEVER opened, no live WorldMapViewModel.

The RE (windows_worldmap_affine_resident_source_re_findings.md) proved every converter field is
exe-invariant (bias {128,128} + scale 1.0 = .rdata, origin = a zeroed .data global, keys = ctor
immediates) — only the legacy-dungeon fold is param-driven (already resident via goblin::legacy_fold).
So the base affine can be rebuilt off-VM and run through the engine's own per-converter projection
(FUN_140876140 / WORLDMAP_PROJ_POINT, which AOB-resolves map-closed). The one open caveat the findings
flag: the STATIC origin reads 0, but our proven-working baked affine has origin 7168/16384 — the offset
may live in the grid decode (area-local frame) instead. This test settles it EMPIRICALLY.

Two independent checks:
  (1) NEVER-OPENED: with the map never opened this session, project 60/42/36 off-VM under BOTH candidate
      constructions and see which reproduces the known reference (u=3712 v=7296):
        - preset "unified": origin 7168/16384, gridbase 0/0
        - preset "static" : origin 0/0,       gridbase 28/64  (28*256=7168, 64*256=16384)
      Whichever matches settles the static-origin-0 vs baked-7168 caveat.
  (2) CAPTURE-REPLAY: open the map once (resolve the VM), capture the LIVE converter fields + a LIVE
      `proj` ground truth, CLOSE the map, replay the captured fields through proj_conv (off-VM) and
      assert du/dv==0. Proves a self-built slot reproduces the engine slot byte-for-byte.

Run: python tools/rpc_tests/test_converter_offvm.py
"""
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mfg_session import run_test  # noqa: E402

REF_AREA, REF_GX, REF_GZ = 60, 42, 36  # findings reference point: proj 60 42 36 -> u=3712 v=7296


def _proj(g, area, gx, gz, px=None, pz=None):
    extra = f" {px} {pz}" if px is not None else ""
    r = g.rpc(f"proj {area} {gx} {gz}{extra}")
    m = re.search(r"u=(-?[\d.]+) v=(-?[\d.]+) page=(-?\d+)", r)
    return (float(m.group(1)), float(m.group(2)), int(m.group(3))) if m else None, r


def _proj_nvm(g, area, gx, gz, px=None, pz=None):
    extra = f" {px} {pz}" if px is not None else ""
    r = g.rpc(f"proj_nvm {area} {gx} {gz}{extra}")
    m = re.search(r"u=(-?[\d.]+) v=(-?[\d.]+) page=(-?\d+)", r)
    return (float(m.group(1)), float(m.group(2)), int(m.group(3))) if m else None, r


def _proj_conv(g, area, gxb, gzb, ox, oz, bx, bz, sc, gx, gz):
    r = g.rpc(f"proj_conv {area} {gxb} {gzb} {ox} {oz} {bx} {bz} {sc} {gx} {gz}")
    m = re.search(r"u=(-?[\d.]+) v=(-?[\d.]+)", r)
    return (float(m.group(1)), float(m.group(2))) if m else None, r


def _conv_affine(g, area):
    r = g.rpc(f"conv_affine {area}")
    m = re.search(r"gxbase=(-?\d+) gzbase=(-?\d+) origin=(-?[\d.]+),(-?[\d.]+) "
                  r"bias=(-?[\d.]+),(-?[\d.]+) scale=(-?[\d.]+)", r)
    if not m:
        return None, r
    return dict(gxb=int(m.group(1)), gzb=int(m.group(2)), ox=float(m.group(3)), oz=float(m.group(4)),
                bx=float(m.group(5)), bz=float(m.group(6)), sc=float(m.group(7))), r


def _test(g):
    g.load_save()
    g.check("alive in-world", g.alive())
    g.rpc("set rpc_auto_idle false")

    # ── (1) NEVER-OPENED: the map has not been opened this session; FUN_140876140 AOB-resolves anyway. ──
    uni, uni_r = _proj_conv(g, REF_AREA, 0, 0, 7168, 16384, 128, 128, 1, REF_GX, REF_GZ)
    sta, sta_r = _proj_conv(g, REF_AREA, 28, 64, 0, 0, 128, 128, 1, REF_GX, REF_GZ)
    g.check("off-VM proj_conv runs map-CLOSED (unified preset)", uni is not None, uni_r)
    g.check("off-VM proj_conv runs map-CLOSED (static preset)", sta is not None, sta_r)

    def _hit(uv):
        return uv is not None and abs(uv[0] - 3712.0) < 0.5 and abs(uv[1] - 7296.0) < 0.5

    which = "unified" if _hit(uni) else ("static" if _hit(sta) else "NEITHER")
    g.check("a map-CLOSED off-VM construction reproduces the reference u=3712 v=7296",
            _hit(uni) or _hit(sta), f"unified={uni} static={sta} -> matched={which}")
    print(f"[OFFVM] map-closed construction that reproduces the affine: {which} "
          f"(unified={uni} static={sta})", flush=True)

    # The SHIPPED path: project() itself must now succeed with the map NEVER opened (off-VM fallback for
    # the base areas). This is what drops the "silent prime" — the vmap can project before any map-open.
    nvm_uv, nvm_r = _proj(g, REF_AREA, REF_GX, REF_GZ)
    g.check("project() works map-NEVER-opened via off-VM fallback (prime dropped)",
            nvm_uv is not None and abs(nvm_uv[0] - 3712.0) < 0.5 and abs(nvm_uv[1] - 7296.0) < 0.5, nvm_r)

    # ── (2) CAPTURE-REPLAY: open once, capture live fields + live proj, close, replay off-VM. ──
    open_uv = None
    for _ in range(4):
        g.rpc("key m"); time.sleep(3)
        g.rpc("mouse_move 960 540"); time.sleep(0.3)
        g.rpc("mouse_drag 1000 560 820 460"); time.sleep(0.8)
        open_uv, open_r = _proj(g, REF_AREA, REF_GX, REF_GZ)
        if open_uv is not None:
            break
    g.check("live converter resolves once the map is opened", open_uv is not None, open_r)

    aff, aff_r = _conv_affine(g, REF_AREA)
    g.check("captured live converter fields", aff is not None, aff_r)

    # Close the map. Press Escape only while it is STILL open — pressing again after it closes pops the
    # system menu (menucover=1) and the loop never settles. map_open==0 is the residency-relevant state.
    closed = False
    for _ in range(8):
        st = g.status()
        if st.get("map_open") == 0:
            closed = True
            break
        g.rpc("key Escape"); time.sleep(1.5)
    g.check("native map closed (map_open==0)", closed, str(g.status()))

    if aff and open_uv:
        rep, rep_r = _proj_conv(g, REF_AREA, aff["gxb"], aff["gzb"], aff["ox"], aff["oz"],
                                aff["bx"], aff["bz"], aff["sc"], REF_GX, REF_GZ)
        g.check("off-VM replay of captured fields runs map-closed", rep is not None, rep_r)
        if rep:
            du = abs(open_uv[0] - rep[0]) + abs(open_uv[1] - rep[1])
            g.check("off-VM replay == live VM proj (du/dv==0) — VM/prime coupling droppable", du < 0.01,
                    f"live={open_uv[:2]} offvm={rep} du={du}")

    # ── (3) EQUIVALENCE: live `proj` (VM path) vs forced `proj_nvm` (off-VM base + legacy_fold) over a
    # sweep of base + legacy-dungeon + DLC-UG samples. The VM is still cached here (persists past close), so
    # `proj` uses the live engine and `proj_nvm` forces the map-closed path — they must AGREE for every
    # sample: both reject (area not placed), or both accept with du/dv<eps AND the same page. This is what
    # extends off-VM coverage past the base areas (fold path). Base samples always place (guaranteed
    # exercise); legacy/DLC-UG samples exercise the fold only where the game actually places that block. ──
    samples = [
        (60, 42, 36, 0.0, 0.0, "overworld base"),
        (61, 40, 40, 0.0, 0.0, "DLC overworld base"),
        (12, 1, 0, 50.0, 50.0, "base underground"),
        (11, 0, 0, 0.0, 0.0, "legacy m11 (Leyndell)"),
        (10, 0, 0, 0.0, 0.0, "legacy m10 (Stormveil)"),
        (30, 0, 0, 0.0, 0.0, "minor dungeon m30"),
        (35, 0, 0, 0.0, 0.0, "legacy chain m35"),
        (40, 1, 0, 50.0, 50.0, "DLC underground m40"),
    ]
    placed = 0
    agree = True
    detail = []
    for area, gx, gz, px, pz, what in samples:
        lv, _ = _proj(g, area, gx, gz, px, pz)
        nv, _ = _proj_nvm(g, area, gx, gz, px, pz)
        if (lv is None) != (nv is None):
            agree = False
            detail.append(f"{what}: DISAGREE placed live={lv is not None} nvm={nv is not None}")
        elif lv is not None:
            placed += 1
            du = abs(lv[0] - nv[0]) + abs(lv[1] - nv[1])
            if du >= 0.5 or lv[2] != nv[2]:
                agree = False
                detail.append(f"{what}: MISMATCH live={lv} nvm={nv} du={du}")
    g.check("live proj == forced off-VM proj_nvm over base+legacy+DLC-UG samples (fold coverage)",
            agree, f"placed={placed}/{len(samples)}; " + ("; ".join(detail) if detail else "all agree"))
    print(f"[OFFVM] equivalence sweep: {placed}/{len(samples)} samples placed; "
          f"{'ALL AGREE' if agree else 'DISAGREEMENTS: ' + '; '.join(detail)}", flush=True)


SWEEP = _test  # single-boot, self-loads


if __name__ == "__main__":
    run_test(_test)
