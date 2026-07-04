# Surviving an ELDEN RING patch — build fingerprint + binary-diff recovery

Every fixed RVA and AOB in [`re_signatures.hpp`](../../src/re_signatures.hpp) (+ the
[RVA-hardening backlog](rva_aob_hardening_backlog.md)) is pinned to **one** `eldenring.exe` build
(the dev box = ERR 2.2.9.6, imagebase `0x140000000`). A Steam update silently invalidates all of them.
This doc is the maintenance recipe: how to **detect** a build change and how to **recover** the pinned
sites when one happens.

## The silent-failure modes (why this matters)

A game update breaks RE work in three ways, none of which announce themselves:

1. **Stale Ghidra project** — Ghidra analyses the dump you imported *earlier*. After an update the running
   exe ≠ that dump, so you decompile the OLD build and derive RVAs that don't match live. No warning.
2. **Hardcoded RVAs go wrong** — `CASTRAY_RVA 0xc70360`, `PHYSWORLD_RVA 0x3d76060`, the two **hooked**
   grace-suppression fns, etc. point into a shifted image → call into the middle of another function →
   crash or garbage.
3. **The decrypted dump itself** — the on-disk exe is VMProtect/Steam-wrapped, so the Ghidra input is a
   *runtime dump*. Two builds = two dumps; mixed under one program name, you annotate build A while looking
   at build B.

## Guard #1 (the practical one): pin the game version

ERR is locked to a specific ER version — running it on a newer ER usually breaks the mod anyway. So on the
dev box:

- Steam → ELDEN RING → Properties → Updates → **"Only update this game when I launch it"**, and launch via
  the mod launcher (or keep Steam offline). This makes an accidental mid-work update nearly impossible.
- Record the pinned version (below) so a drift is visible even if the setting is ever missed.

## Guard #2 (verifiable): the build fingerprint

The DLL reads `eldenring.exe`'s `VS_FIXEDFILEINFO` version and surfaces it two ways
([`goblin_build_id.hpp`](../../src/goblin_build_id.hpp)):

- **Boot log** — `[BUILD] eldenring.exe version = a.b.c.d`, logged right before the AOB PASS/FAIL health
  check. The version is the **header** for reading those PASS/FAIL lines: a fresh FAIL after an *unexpected*
  version bump = Steam updated the game under the pinned signatures.
- **Live RPC** — `er_version` (twin of `er_base`): `python tools/mfg.py rpc er_version` →
  `ok er_version=a.b.c.d`. Verify it matches the build your RVAs came from before trusting an RVA-derived
  address (e.g. before running `tools/hf_hook_scout.py`).

The version resource lives in the PE resource directory, which VMProtect/Steam-DRM leave intact, so this is
the *real* game version off the on-disk exe (not something the packer mangles).

> **Pinned build (update on a deliberate bump):** ERR 2.2.9.6 — record the exact `er_version` string here
> the first time it's read on the dev box so a future mismatch is obvious.

## Recovery: binary-diff the two builds (when a patch lands anyway)

Binary diffing matches functions across two builds and yields the `old RVA → new RVA` map + which function
**bodies** changed (AOB needs re-crafting) vs merely **moved** (RVA changed, AOB still fine). It also
auto-ports Ghidra annotations. Tools: **BinDiff** (free, via BinExport), **Diaphora** (open-source, Ghidra),
or Ghidra's built-in **Version Tracking**.

**⚠ The VMProtect trap — do NOT diff the on-disk exes.** The on-disk `eldenring.exe` is encrypted/packed;
its bytes are noise (the packer layout shuffles even for identical logical code, and most real code is
encrypted). **You must diff the runtime-*decrypted* dumps** of both builds. Concretely:

1. **Always keep a reference dump.** Archive the decrypted dump of the *current* pinned build + its
   `er_version` and SHA-256, so there is always an "A" to diff against. (Stamp the version into the dump
   filename and the Ghidra program name — that alone kills failure mode #3.)
2. **On a patch:** dump build "B" from live memory the same way, then BinDiff/Diaphora **A ↔ B**.
3. **Port + remap:** carry annotations A→B; read off the RVA remap for the pinned sites.
4. **Regenerate broken AOBs:** for any site whose body changed, re-craft the AOB from live bytes with
   `mem_dump er+<newRVA>` and the craft recipe in [`rva_aob_hardening_backlog.md`](rva_aob_hardening_backlog.md)
   ("How to harden"). Update `re_signatures.hpp`, re-check the `[SIG]` PASS/FAIL log, and bump the pinned
   version above.

## The durable answer (bigger than diffing)

Diffing is **recovery**, not defense. The real defense is the existing **AOB doctrine**: an AOB matches
runtime bytes and survives most patches with no diff at all. So the highest-value ongoing work is finishing
the [RVA-hardening backlog](rva_aob_hardening_backlog.md) (turn RVA-only resolutions into AOBs) so that a
patch mostly self-heals; the diff is only for the residue that genuinely moved bytes. See `common.md` for
the AOB doctrine.
