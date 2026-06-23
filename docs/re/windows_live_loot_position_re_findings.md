# Findings — live loot/treasure WORLD position

Companion to `windows_live_loot_position_re_prompt.md`.

---

## §0 pre-check — do baked loot positions drift vs the current ERR MSB? → **NO (verdict: SKIP the RE)**

**One-line:** A virgin re-extract from the current ERR MSB reproduces every committed baked loot
position **exactly** (0 of 30,305 shared placements moved). Baked positions are faithful; the
runtime live-position RE buys design-purity only, not correctness — **do not start it now.**

**Method (offline, no RE):** re-ran `tools/extract_all_items.py` against the on-disk ERR mod
(`D:\DOWNLOAD\ERR_mod`, ERR 2.2.9.6 — the same version the committed bake came from), then diffed
positions against the committed `data/items_database.json` (pulled from git HEAD). Join key =
`(map, partName, itemLotId, source)` over records carrying a real MSB `partName` (a physical
placement); compared `(x,y,z,areaNo,gridX,gridZ)`.

| metric | value |
|---|---|
| records total (baseline = fresh) | 31089 |
| placed records (have partName) | baseline 30341 / fresh 30305 |
| shared placement keys | 30305 |
| **positions moved** | **0** |
| only in baseline | 36 (EMEVD enrich-fallback rows added *after* extraction — virgin extract is pre-enrich; not moved chests) |
| only in fresh | 0 |

`npc_name_ids.json` and `unreachable_msb_lots.json` also re-extracted byte-for-byte.

**Caveat — what this does and does NOT prove:** only ERR 2.2.9.6 is on disk (= the bake's
version), so this confirms the extraction is **faithful and deterministic** for the current
version. It cannot prove a *future* ERR version won't relocate chests. But the §0 reasoning holds:
ERR is additive and the randomizer shuffles lot *contents*, never the chest/enemy placement — so
position drift is not expected even across versions.

**Decision:** position stays **baked**; the live-position RE (treasure/asset placement system,
`WORLD_GEOM_MAN_SLOT` walk, enemy-drop ChrIns) is **not justified** — it's HIGH effort for a drift
that measures zero. The cheap, real future-proof is to **re-run this diff on each ERR version
bump** (a position-coverage check), not a runtime read. Revisit the RE only if a future bump shows
many placements moved, or if full runtime-purity (zero baked loot data) becomes an explicit goal.

The reproducible diff lives in this session's command log (git-HEAD baseline vs `extract_all_items`
re-run); fold it into a `tools/diag_loot_position_drift.py` if it needs to run per version bump.
