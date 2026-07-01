# Findings — MapGenie category coverage verification

Companion to `windows_mapgenie_category_coverage_re_prompt.md` and
`docs/plans/mapgenie_category_coverage_plan.md`. Each entry: verified field + value(s),
the real row(s) used, and CONFIRMED / WRONG vs the plan's hypothesis.

Method note: verification reads `regulation.bin` **off disk** via SoulsFormats
(`tools/verify_disablerespawn.py`). Params are baked verbatim into `regulation.bin` and the
game loads that same table into memory at boot, so a disk read is value-identical to the live
`from::params::get_param` path — no running game or memory attach needed. Params are NOT
streamed; every row is resident regardless of the player's map. (Streaming only applies to MSB
placement + textures, which this tier does not touch.)

---

## Part A(a) — `WorldFarmableEnemy` — PARTIALLY WRONG (gate needs a 2nd condition)

**Field:** `NpcParam.disableRespawn` (u8 bitfield `:1`, JP "リスポン禁止か" = "respawn prohibited?").
**Tool:** `tools/verify_disablerespawn.py` (run for `err` and `MFG_PROFILE=vanilla`).

**Distribution (mod-agnostic — same semantics both installs):**
- vanilla: 6713 rows `dr=0`, 326 rows `dr=1` (of 7039)
- ERR:     6435 rows `dr=0`, 433 rows `dr=1` (of 6868)

**Verified polarity:**
- **`dr=1` → reliably one-time / NOT farmable. CONFIRMED.** The `dr=1` set is invaders, questline
  NPCs, and event/prop dummy rows — e.g. Melina `21801000`, Millicent `523480156`, Sir Ansbach
  `524160199`, Needle Knight Leda `524180189`, Festering Fingerprint Vyke `523040020`, Mad Tongue
  Alberich `523850000`, Blaidd's one-time fight `20109140`, Castellan Edgar boss-version
  `533110000`, plus non-enemy props ("Smithing Table" `500010000`, "Twin Maiden Husks"
  `500020079`). None are respawning trash.
- **`dr=0` → NOT a farmable signal. HYPOTHESIS WRONG.** The main fog-gated bosses read `dr=0` in
  **both** vanilla and ERR: Rennala `20300000` (vanilla) / `20300024` (ERR), Draconic Tree Sentinel
  `32500072`, etc. Their non-respawn is enforced by the boss-defeat **event flag + fog gate**, not by
  `disableRespawn`. So `dr=0` over-includes bosses.

**Consequence for implementation:** the plan's assumption that "`disableRespawn` should always be 1
for bosses anyway" is **false** — bosses read 0. `disableRespawn` alone is not a safe gate. Correct
gate:

    WorldFarmableEnemy  ⇔  disableRespawn == 0  AND  NOT already classified as boss/named-NPC

i.e. reuse the existing `WorldBosses` / named-NPC classification in `goblin_inject.cpp` to exclude
those first, then treat `dr==0` on the remaining trash as farmable. `dr==1` can be used as a hard
"never farmable" short-circuit (reliable in that direction).

**Row-identity caveat:** the same name can map to two NpcParam rows with different `dr` (Castellan
Edgar `523110000` dr=0 friendly vs `533110000` dr=1 boss). The classifier must read the
`NPCParamID` of the *placed* enemy instance, not a name lookup.

**Confidence:** HIGH. Reproducible via `tools/verify_disablerespawn.py`, matches across vanilla+ERR.

---

## Part A(b) — `WorldFarmableCollectible` — HYPOTHESIS WRONG (wrong field), corrected + simpler

**Tool:** `tools/verify_farmable_collectible.py` (run for `err` and `MFG_PROFILE=vanilla`).

**The plan's field is wrong.** It cited `getItemFlagId01/02/03` ("any of 3 slots == 0"). The paramdef
(`tools/paramdefs/ItemLotParam.xml`) actually has **8** per-slot fields `getItemFlagId01..08` (one per
lot item slot) PLUS a single master `getItemFlagId`. And empirically:

- **Per-slot `01..08` are ALWAYS 0** — 0 lots out of the full table have any nonzero per-slot flag, in
  BOTH `ItemLotParam_map` and `ItemLotParam_enemy`, on BOTH vanilla and ERR. They are unused override
  slots (paramdef: "0 = 共通使用" = "0 = use the shared/master one"). So "any of N slots == 0" is a
  non-signal — checking them is pointless.
- **The authoritative field is the single master `getItemFlagId`** (paramdef "0 = フラグ無効" =
  "0 = flag disabled"). This is exactly what the existing tool `tools/dump_loot_flags.py` already reads,
  and what the live overlay path already reads at `goblin_inject.cpp:4720`
  (`getItemFlagId @ +0x80`, commented "lot-wide … authoritative").

**Polarity CONFIRMED:** `getItemFlagId == 0` → no persistent acquire flag → farmable; `!= 0` → tracked.
Nonzero rows are the one-time uniques (Larval Tear `flag=12017985`, Dragon Communion Seal, Omen Bairn,
Immunizing Horn Charm); zero rows are re-rollable enemy/material drops (Old Fang, Neutralizing Boluses).

**Distribution (sanity-consistent):**
- vanilla: `ItemLotParam_map` 517 farmable / 5047 tracked · `ItemLotParam_enemy` 4891 farmable / 244 tracked
- ERR:     `ItemLotParam_map` 2094 / 7128 · `ItemLotParam_enemy` 5369 / 477
- Most MAP lots are tracked (world pickups are grab-once); most ENEMY lots are farmable (trash re-drops).

**Refinement — nonzero is NOT automatically "one-time" (live-only distinction):** `goblin_inject.cpp`
already distinguishes *persistent* one-time flags from *repeatable* flags (nonzero flags with no
save-backed obtained bit) via `flag_is_repeatable()` (live group-allocation query, `:4679`, used at
`:4736`/`:4799`). A repeatable nonzero flag is still effectively farmable. So the correct farmable test
is not `flag == 0` alone but:

    WorldFarmableCollectible  ⇔  getItemFlagId == 0  OR  flag_is_repeatable(getItemFlagId)

This is a static-vs-live gap: the disk read sees zero/nonzero; the repeatable-vs-persistent split of the
nonzero set needs the runtime query. Not a blocker — the live path already computes exactly this.

**Implementation impact:** SIMPLER than the plan assumed. The field is already read live at the loot
site (`resolve_loot_flag`, `:4700`) and `flag_is_repeatable` already exists — `WorldFarmableCollectible`
needs zero new param plumbing, just a category branch reusing that resolved value.

**Composition with A(a):** enemy-drop collectible is farmable only if the enemy also respawns —
`enemy.disableRespawn == 0` (A(a)) AND lot farmable. Map gathering nodes: lot farmable alone.

**Row-noise caveat:** the lowest lot IDs (0/1/2/100/…) are internal/template rows (e.g. the starting
Flask of Crimson Tears at lot 2, `flag=0`), not world placements. Only lots referenced by a placed
asset/enemy reach the classifier, so this template noise is irrelevant in practice.

**Confidence:** HIGH. Reproducible via `tools/verify_farmable_collectible.py`; the authoritative offset
+0x80 is already live-cross-validated in shipped code.

## Tier 2 — `WorldMapPointParam.iconId` landmarks — PENDING

## Tier 3 — `NpcParam` teamType/npcType — PENDING (shares the NpcParam read site with A(a))

## Tier 4 — Lore / Miscellaneous / Quest — PENDING (Quest = spot-check only)
