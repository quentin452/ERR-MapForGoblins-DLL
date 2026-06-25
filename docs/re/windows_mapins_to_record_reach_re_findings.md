# Findings — MapIns → loot-record reach (LIVE)

Answers `windows_mapins_to_record_LIVE_re_prompt.md` (commit b96c04f). Live RPM on the running
`eldenring.exe` (pid this session), scripts `D:\ghidra_scripts\live_reach.py` + `mapins_reach_check.py`.

> ⚠️ Corrects the prompt's two leads: (1) `er+0x485cbb8` was NOT WorldMapManImp — **abandoned**;
> (2) "record NOT in body" was a scan-range artifact — **the record IS reachable at `MapIns+0x460`**
> (the body walk just didn't reach that far). The deterministic reach below is via the proven
> vtable-scan enumeration, not a singleton chain.

---

## ★ DETERMINISTIC REACH — `MapIns + 0x460`, self-validating
Enumerate MapIns by **vtable-scan** (`er+0x2a8d6d8`, from `tools/ghidra/rtti_index.txt`; 343 resident,
patch-stable). For each MapIns instance `m`:

```
node   = m + 0x460                      # the loot record, inline
  lotId   = u32 @ node+0x00
  flag    = u32 @ node+0x04             # small (1)
  FieldIns* = ptr @ node+0x08
VALIDATE: *(u32)(FieldIns + 0x50) == lotId      # integrity gate — rejects every false positive
MapId    = u32 @ m + 0x388              # = node-0xD8   (e.g. 0x3c253200 = m60_37_50_00)
localPos = vec3 @ m + 0x38c             # = node-0xD4
absPos   = (gridX*256+localX, localY, gridZ*256+localZ)   # gridX=(MapId>>16)&0xff, gridZ=(MapId>>8)&0xff
```
Live confirm (`mapins_reach_check.py`, this session): walking all 343 MapIns and reading `+0x460` with
the validation gate yields the chest `AEG099_090_9000`:
`lot=1037500100 (m60_37_50_00) pos=(56.3,238.1,52.7) ABS=(9528,238,12853) F=0x29cb94b31c0 OK`.
`lotId → resolve_loot_item_textid` gives the name. Cross-session stable: `MapIns−0x460` reach matches
§7 (different process, same offset).

**Why it's safe even though +0x460 is single-sampled:** the `*(FieldIns+0x50)==lotId` gate means the
walker only ever emits a record it has *proven* is a real loot node. Reading `+0x460` on a
non-item MapIns yields junk that fails the gate and is skipped. Worst case = under-report (an item whose
record sits at a different offset is missed), never garbage. So it can ship now.

## ★★ The make-or-break, re-confirmed live: only SPAWNED/OPENED loot is resident
- `live_reach.py` full-mem scan for the "アイテム" FieldIns name (`30A2 30A4 30C6 30E0`): **11 string
  hits, but only 1 is a real loot node** (the rest are non-FieldIns text). `mapins_reach_check.py`:
  **1/343 MapIns** item-bearing. That one is the chest the player has **opened**.
- This is exactly §8's verdict: sealed chests carry no resident record; only spawned/opened/placed loot
  does. So the reach is correct but the **scope is loaded-and-spawned loot only**.

## Generalisation status (honest)
- The reach (`+0x460`, MapId/pos at `−0xD8/−0xD4`) is confirmed on **1 node across 2 sessions** — not yet
  over ≥3 nodes / ≥2 maps, because **only 1 item-bearing MapIns is resident** in this game state (the
  single opened chest). Cannot fabricate more without the player opening chests / standing near dropped
  items — out of RE control.
- Robustness hedge for the mod walker (covers the case where `+0x460` is object-specific rather than a
  fixed MapIns field): in addition to `+0x460`, scan a bounded window of the MapIns body
  (`+0x100 .. +0x800`, 4-aligned) for the node signature, each candidate gated by `*(FieldIns+0x50)==L`.
  The gate keeps it false-positive-free; the window widens coverage if other items sit at other offsets.
  Coverage then self-reveals as the player explores (the in-tree `[MAPINS]` walker logs it).

## Net / wiring
- **Enumeration:** vtable-scan `er+0x2a8d6d8` (343). **Reach:** `MapIns+0x460` node, gated by
  `FieldIns+0x50==lotId`. **Position:** MapId `m+0x388` + localPos `m+0x38c` → absolute. **Identity:**
  `lotId → resolve_loot_item_textid`. All live-proven on the available sample.
- The in-tree `[MAPINS]` walker (`goblin_collected.cpp`, diag commits) needs only these offsets:
  read `+0x460` (+ optional body-window), validate, emit `(lotId, MapId, localPos)`.
- Scope = loaded spawned/placed loot (sealed chests stay baked-only, §8). For the explore-cache premium
  this is the viable layer: ERR-added world-placed loot + opened/dropped items, with names + absolute pos.

Tooling: `live_reach.py`, `mapins_reach_check.py`, `mapins_enum.py`, `tools/ghidra/{rtti_index.txt,query.java}`.
