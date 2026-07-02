# Enemy healthbar NAMES — RE findings (mob names on the game's existing HP bar)

**Goal (user, 2026-07-03):** show the mob NAME on ELDEN RING's *already existing* enemy health bar.
Vanilla draws the bar for every locked/aggroed enemy and shows the NAME only for BOSSES; regular
mobs get the bar but no name. Scope = add the name to the non-boss (entity) HP bar. No new bar, no
world→screen projection, no HP-field RE.

## The key discovery — the game already hands us the on-screen bar position

ERR bundles **`PostureBarMod.dll`** (Mordrog, MIT-ish open source,
`github.com/Mordrog/EldenRing-PostureBarMod`), ACTIVE in `err_offline.me3`, ImGui+DX12 like us. It
draws posture bars *offset from each enemy's health bar position* — i.e. it already located the
game's own per-entity HP-bar. Its mechanism is exactly the access we lacked (live enemy pointer +
screen position). Offsets below are taken from its source AND are **live-valid on this exact ERR
build** because the bundled dll works in-process (bars render).

**`CSFeMan` (front-end / HUD manager singleton) owns an array of HP-bar structs, each already
projected to screen by the engine.** We do NOT enumerate WorldChrMan ourselves and do NOT project —
we read the engine's populated array every frame.

## Struct map (ER version = whatever ERR v2.2.9.6 ships; validated by the working PostureBarMod dll)

```
CSFeManImp:
    +0x59F0  EntityHpBar entityHpBars[8]   // ENTITY_CHR_ARRAY_LEN = 8
    +0x5BF0  BossHpBar   bossHpBars[3]     // BOSS_CHR_ARRAY_LEN  = 3  (0x59F0 + 8*0x40)

EntityHpBar (size 0x40):
    +0x00  u64   entityHandle      // == UINT64_MAX (0xFFFF...) when the slot is empty
    +0x08  float unk[2]
    +0x10  float screenPosX        // engine-computed, in the game's 1920x1080 virtual coord space
    +0x14  float screenPosY
    +0x18  float mod (distanceModifier)
    +0x34  bool  isVisible         // field order: mod(0x18) unk3(0x1C) unkChar1/2+unkShort3(0x20)
                                    // currentDisplayDamage(0x24) previousHp(0x28) timeDisplayed(0x2C)
                                    // totalTimeDisplayed(0x30) isVisible(0x34) unk2[11] -> sizeof 0x40

BossHpBar (size 0x20):
    +0x00  int  displayId          // boss NpcName/display id — vanilla already shows this
    +0x08  u64  bossHandle         // == UINT64_MAX when empty

ChrIns:
    +0x08  u64  handle
    +0x60  int  npcParam           // <<< the npcParamId — the whole feature hinges on this
    +0x64  int  modelNumber
    +0x68  int  chrType
    +0x6C  u8   teamType
    +0x190 ChrModuleBag* chrModulelBag  // ->statModule(health@+0x138), ->staggerModule(staggerMax)

WorldChrMan:
    +0x10EF8  ChrIns** playerArray[4]     // playerArray[0] = LocalPlayer chain
```

## Signatures (from PostureBarMod Hooking.cpp — AOB, ER build-specific)

- **CSFeMan slot** (RIP+3): `48 8B 0D ? ? ? ? 8B DA 48 85 C9 75 ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 4C 8B C8 4C 8D 05 ? ? ? ? BA B4 00 00 00 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 8B D3 E8 ? ? ? ? 48 8B D8`
- **GetChrInsFromHandle** (function ptr, use scan address directly):
  `48 83 EC 28 E8 17 FF FF FF 48 85 C0 74 08 48 8B 00 48 83 C4 28 C3`
  signature: `ChrIns* (*)(WorldChrMan* wcm, u64* handlePtr)`
- **WorldChrMan** — we ALREADY resolve this (`goblin_world_position.cpp` `g_wcm_static`,
  `er+0x3D65F88` / `WCM_FINDER` AOB). Reuse it; `*g_wcm_static` = WorldChrMan*.
- **UpdateUIBarStructs** — PostureBarMod HOOKS this to snapshot the array at the game's update time.
  We do NOT need the hook: the array persists frame-to-frame, so we just READ `*csFeMan +0x59F0`
  in our present/render pass. (Sig kept for reference:
  `40 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 C7 44 24 30 FE FF FF FF 48 89 9C 24 B0 00 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 58 48`)

## The read loop (per frame, host side — reads game memory)

```
wcm    = *g_wcm_static                       // WorldChrMan*
feMan  = *g_csfeman_slot                      // CSFeManImp*
for i in 0..8:
    e = feMan->entityHpBars[i]
    if e.entityHandle == UINT64_MAX || !e.isVisible: continue
    chr = GetChrInsFromHandle(wcm, &e.entityHandle)   // SEH-guard the call
    if !chr: continue
    npcId = chr->npcParam                     // ChrIns+0x60
    // name (reuse existing mod-agnostic path):
    npc_team_and_name(npcId, &team, &nameId)  // src/goblin_item_classify.cpp:286
    name = lookup_text_utf8(nameId + 700000000)   // NpcName FMG band
    emit { screenPosX, screenPosY, name }     // to the render side
```

Boss bars (`bossHpBars[i].bossHandle` / `.displayId`) are left alone: vanilla already names bosses.

## Integration notes for THIS repo (split host/render DLL)

- **Host side** (reads game memory): resolve `g_csfeman_slot` (add the CSFeMan sig) +
  `GetChrInsFromHandle` (add sig + typedef); reuse `g_wcm_static`. Reuse `npc_team_and_name`
  (`goblin_item_classify.cpp:286`) + `lookup_text_utf8` (`goblin_messages.cpp:684`). SEH-guard every
  game-pointer deref (statics are null mid-load — mirror `probe_player_seh`).
- **Verify our existing `fe_man_slot`** (`goblin_inject.cpp:189`, resolved via
  `EVENT_FLAG_MAN_SLOT_ALT`, logged as `CSFeMan_slot`): it MAY already be the same CSFeMan instance.
  Log `*fe_man_slot` vs `*g_csfeman_slot` (new dedicated sig) once — if equal, drop the new sig and
  reuse `fe_man_slot`. Until confirmed, use the dedicated PostureBarMod CSFeMan sig (its +0x59F0
  offset is only valid against the instance that sig resolves).
- **Render side** (draws): expose the emitted list via a `GOBLIN_RENDER_API` getter (mirror
  `get_player_facing_yaw` plumbing) and draw `ImGui` text at each `(screenPosX, screenPosY)` scaled
  from the game's 1920x1080 virtual space to the backbuffer (reuse the overlay's existing
  resolution-scale — same virtual canvas the day/night dial exclusion uses). Offset the text just
  above the bar (bar is ~5px tall; put the name at `screenY - textHeight - 2`).
- **Config**: `enemy_names` toggle (World or a new HUD section). Default ON (user asked for it).
- **Diagnostic**: gate a `[ENEMYBAR]` log (entityHandle/screenPos/npcId/name per visible slot) on
  `debug_logging` to validate the offsets live before trusting the draw.

## Offset-drift caveat

These offsets are the ER version ERR v2.2.9.6 ships. If ERR updates the game exe, PostureBarMod's
own dll would break too — so a broken PostureBarMod (bars gone) is the canary that these offsets
moved; re-pull PostureBarMod source at that version. The name path (`npc_team_and_name` → param
table + FMG) is version-robust (uses our AOB-resolved param/FMG infra), only the CSFeMan/ChrIns
raw offsets are version-pinned.

## Credit

Struct layout + signatures derived from **Mordrog/EldenRing-PostureBarMod** (open source). We
reimplement the READ (no code copied beyond the ABI facts); PostureBarMod remains the upstream
reference for the HP-bar HUD structs.
