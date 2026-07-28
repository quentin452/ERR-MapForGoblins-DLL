# Enemy names: why vanilla shows almost none and ERR shows them all

**Status: NOT A BUG — measured 2026-07-28.** Recurring question ("the enemy name tag works on ERR
but only bosses get one on vanilla"). The answer is a data difference in the install, not a defect
in `src/goblin_enemy_names.cpp`.

## How the name gets on screen

The mod does **not** draw its own bar. It resolves a name and injects it into the game's own
`NpcParam.nameId -> NpcName` path, so the **engine** draws its native red enemy tag. No name
resolved ⇒ no `nameId` injected ⇒ the engine draws *nothing at all* — no tag, no bar. That is the
whole mechanism, and it is why "no name" looks like "no enemy bar".

Resolution order (`src/goblin_enemy_names.cpp` header):

| tier | source | covers |
|---|---|---|
| 4 | EMEVD boss health bar `2003[11]` | bosses, per-ENCOUNTER, authoritative (follows a mod's reskin/rename) |
| 1 | `NpcParam.nameId` → `NpcName` | named entities: NPCs, invaders, some minibosses |
| 2 | `TutorialTitle` bestiary codex | **every generic enemy — but only if the install ships a codex** |
| 3 | `NpcName` boss band `9e8 + model*1000` | vanilla field bosses whose `nameId` is 0 (e.g. Tree Sentinel) |
| — | else | nameless → draw nothing |

## The measurement (2026-07-28, this box)

Vanilla = `E:\SteamLibrary\...\ELDEN RING\Game`; ERR = the `mod/` overlay of ERRv2.2.9.6. Both read
from their own `regulation.bin` + `msg/engus/menu_dlc02.msgbnd.dcx`:

| | tier 1 — `NpcParam.nameId != 0` | tier 2 — `TutorialTitle` entries |
|---|--:|--:|
| **Vanilla** | 716 / 7039 (10 %) | **78** |
| **ERR** | 935 / 6868 (14 %) | **3218** |

Vanilla's 78 `TutorialTitle` entries are the actual tutorial pop-ups. **ERR's 3218 are the Reforged
bestiary codex** — a 41× difference, and the entire explanation. Tier 2 is the only tier that names
generic enemies, and vanilla has no data for it.

So on vanilla the only enemies that get a tag are the ~10 % of `NpcParam` rows with a real `nameId`
(NPCs / invaders / bosses) plus what tiers 3-4 catch. Which is exactly what a player observes: "only
bosses have a name".

## Why this is correct, and what NOT to do

Vanilla itself shows no name for a common enemy anywhere in its UI — there is no name to read. The
`else → nameless` branch is marked "vanilla-correct for a true generic" in the code and it is.

- ❌ **Do not embed a name table for vanilla generics.** It is exactly what the mod-agnostic doctrine
  forbids ("derive from the loaded game, never embed a table"), and it would be wrong on every other
  mod — the same chr model is reskinned/renamed freely.
- ✅ **A dev-only fallback to the chr model id** (`c3160`) is doctrine-clean if identification while
  testing on vanilla is ever wanted: it is derived from live data, not a table. Behind a dev toggle,
  never on by default — it is useless to a player.
- ✅ **Default: draw nothing.** Matches the base game.

Related: [[loot-identity-stable-err-additive]] for the same "ERR adds data vanilla does not have"
shape on the loot side. RE background: `docs/re/windows_enemy_name_runtime_source_re_findings.md`,
`docs/re/cross_mod_boss_naming_re_findings.md` (why tier 4 outranks the model-keyed tiers).
