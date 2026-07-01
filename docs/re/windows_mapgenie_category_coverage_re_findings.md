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

## Part A(b) — `WorldFarmableCollectible` — PENDING

`ItemLotParam*.getItemFlagId01/02/03` — not yet verified.

## Tier 2 — `WorldMapPointParam.iconId` landmarks — PENDING

## Tier 3 — `NpcParam` teamType/npcType — PENDING (shares the NpcParam read site with A(a))

## Tier 4 — Lore / Miscellaneous / Quest — PENDING (Quest = spot-check only)
