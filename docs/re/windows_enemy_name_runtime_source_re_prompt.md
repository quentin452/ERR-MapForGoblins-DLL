# RE PROMPT (Windows) — mod-agnostic RUNTIME source for an enemy MODEL → display NAME

## Goal

Find a way to resolve a **live enemy's display name** (e.g. `c3251` → "Tree Sentinel", `c4311` →
whatever it is) **at runtime, from the ACTIVE install's own data** (regulation.bin params + `msg`
FMGs on disk/in-RAM), with **NO bundled/curated table** and **NO hardcoded model→name map**. This
must satisfy the project prime directive ("Runtime/Disk over baked", `AGENTS.md`): it has to produce
a correct name on ANY mod (vanilla, ERR, others) by reading that install's files — not an ERR-frozen
snapshot.

Deliverable: either
- **A mechanism** — the exact param/FMG/function/id-formula chain that maps a live enemy to a
  localized name, reproducible in-DLL (param name + field offset, or FMG slot + id-derivation
  formula, or a game function to call/replicate + its signature), **verified on ≥3 distinct enemy
  models** (a field boss, a generic humanoid, a beast). OR
- **A firm negative verdict**: "no mod-agnostic runtime source exists; a maintained table is
  unavoidable", WITH the best table source identified (see §Fallback) and WHY each runtime avenue
  fails.

## What the Linux session already PROVED (do not re-derive — build on this)

The overlay reads the game's own enemy HP bars (`CSFeMan.entityHpBars[8]`) and, per bar, follows the
handle to the live `ChrIns`. From the live `ChrIns` we can already read, mod-agnostically:

- **npcParamId** at `ChrIns+0x60` (e.g. `32510010`, valid `NpcParam` row).
- **modelNumber** at `ChrIns+0x64` (e.g. `3251`).
- **The live entity "name" string** via the Erd-Tools chain (validated: `Hp@0x138` matches
  PostureBarMod's statModule):
  `moduleBase = [ChrIns+0x190]; enemyData = [moduleBase+0x0]; nameStr(UTF-16) = enemyData+0x1A0`
  → but this string is the **internal part/model name** `"c3251_9000"`, **NOT** a localized name.
  (`Model` UTF-16 sits at `enemyData+0xC8` = `"c3251"`.)

Three candidate name sources were tested on Linux and are **DEAD** for generic enemies:

1. **`NpcParam.nameId` (`+0x0c`, paramdef-confirmed)** = `0` for every generic enemy tested
   (`c3251/c4311/c4600/c6060/c6100`). It is nonzero only for *named* entities (invaders, hostile
   NPCs, some minibosses) — those already resolve fine via `nameId → NpcName FMG`.
2. **`NpcName` FMG** (physical slots `{428, 328, 18}`, our band = `nameId + 700000000`) contains
   **only named characters** — dumped live: "Sorceress Sellen", "White Mask Varré", "Bloody Finger
   Nerijus", "Knight of the Great Jar", ... (ids `13xxxx/14xxxx`). Generic enemy names (e.g. the c3251
   creature) are **not present at any model-derived id** — a probe tried
   `{model, model*10, model*100, model*1000, model*10000, npc, npc/10, ...}` in slots 428/328/18:
   ZERO hits.
3. **Live entity data**: holds the **model/part id string** (`"c3251_9000"`), not a display name
   (see chain above).

Also observed: hooking `MsgRepositoryImp::GetMessage` (the game's own text getter), the ONLY place a
readable "Tree Sentinel" appears is the **field-boss banner**: `fmg=32 id=5763010 'Enter Field Boss:
Tree Sentinel'` (fmg 32 = event/menu text; in ERR the whole phrase is baked into that one entry). The
game **never** looks up a clean per-mob name during normal play.

## The precise question for the Windows RE

**Is there ANY install-resident data path from a live enemy (model `c3251` / npcParam `32510010`) to a
localized display string, without a hand-authored table?** Investigate, in rough priority order:

1. **Other params keyed by model/npc.** We only checked `NpcParam.nameId`. Enumerate params that could
   carry a name id for an enemy: `CharaInitParam`, `NpcThinkParam`, `GameAreaParam` (boss arenas),
   `WwiseValueToStrParam`, `Bullet`/`SpEffect` chains, `MenuPropertyLayoutParam`, or any param whose
   row id is model-derived (e.g. row `3251` / `32510000`) and whose fields include a `*NameId`/`textId`
   that resolves in a name FMG. Deliver: param + row-key formula + field offset + FMG.

2. **The field-boss banner composer (fmg 32, id 5763010).** How does the ENGINE fill "Tree Sentinel"
   into "Enter Field Boss: {name}"? In vanilla ER this is a template with an arg; find where the arg
   (the clean boss name) comes from — a `GameAreaParam`/boss-registration id, an EMEVD `DisplayBanner`
   /`SetBossName` instruction argument, or a text-id passed to the banner call. If the clean name is a
   real FMG entry, get its slot+id and how it links to the boss entity. (Even if this only covers
   field bosses, that's the highest-value subset.)

3. **The engine's target/enemy-name resolver.** Is there a game function `GetChrName(ChrIns*)` /
   name-plate populate that maps a ChrIns to a display string? Reverse it (Ghidra): what does it read
   — a param, an FMG id derived from model, or the same `enemyData+0x1A0` internal string? Its inputs
   are the answer. Provide the signature if a callable/replicable path exists.

4. **NpcName id convention for generic enemies.** Confirm (or refute) that vanilla `NpcName` FMG has
   NO entries for generic field enemies at all (i.e. the wiki names for "Godrick Soldier" etc. are
   community-authored, not shipped). Dump the full `NpcName` FMG (slots 428/328/18) offline from the
   install's `msg\engus\...msgbnd.dcx` and check: is there a "Tree Sentinel"/generic-enemy entry, and
   if so at what id, and is that id derivable from model/npcParam? If a clean formula exists → that is
   the mod-agnostic win.

## Concrete anchors / offsets to reuse

- Enemy access (from `PostureBarMod` + Erd-Tools, live-valid on ERR v2.2.9.6):
  `CSFeManImp+0x59F0 = EntityHpBar[8]` (handle@0, screenPos@0x10/0x14, isVisible@0x34);
  `GetChrInsFromHandle(WorldChrMan*, u64* handle)`; `ChrIns+0x60 npcParam`, `+0x64 model`,
  `+0x190 moduleBase → +0x0 enemyData → +0xC8 Model(str), +0x138 Hp, +0x1A0 nameStr(str)`.
- FMG: our text getter is `MsgRepositoryImp::GetMessage(repo, group=0, fmgSlot, msgId)`; NpcName
  physical slots `{428, 328, 18}`; field-boss banner seen at `fmgSlot=32, id=5763010`.
- Full session detail + the naive-path post-mortem: `docs/re/linux_enemy_healthbar_name_re_findings.md`.
- Runtime FMG/param infra already in the DLL (mod-agnostic disk + resident reads):
  `src/goblin_messages.cpp` (GetMessage, `decode_textid` bands), `src/goblin_item_classify.cpp`
  (`npc_team_and_name`, `from::params::get_param`), `src/from/params.hpp`.

## Environment notes

- **Runtime RE is doable on BOTH boxes.** The whole investigation above ran on **Linux via in-DLL
  probes under Proton** (`src/goblin_param_scan.cpp` VEH+DR0 find-what-accesses, `[GETMSG]`/`[ENEMYBAR]`
  logs). Windows adds: the Ghidra project + offline FMG/param extraction (Oodle decompress works
  in-process on Linux but NOT offline; offline msgbnd/param dumps are the Windows comfort). Use Ghidra
  for §2/§3 (static analysis of the banner composer + the name resolver); use offline msg dumps for §4.
- Reference implementations to mine (open source): **Nordgaren/Erd-Tools** (`Models/Entities/Chr.cs`
  reads the `enemyData+0x1A0` internal name — confirm it has NO localized-name path either) and its
  Debug Tool's "Resources/Params/Names" mechanism (which is exactly the hand-authored table we are
  trying to AVOID — study it to confirm the community consensus, then beat it if a runtime path exists).

## Fallback (if the verdict is "no runtime source")

If no install-resident model→name path exists, the standard is a maintained table (Paramdex NpcParam
row-names, or the community enemy-model list). Deliver: the best single source to import, its
model/npc key convention, coverage (how many models), and how ERR-custom models degrade (fall back to
the raw `cXXXX` id). Note the tradeoff vs the prime directive so the user can decide table-vs-nothing.
