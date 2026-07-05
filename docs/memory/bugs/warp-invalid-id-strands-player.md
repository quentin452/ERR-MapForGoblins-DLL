# Warp to a non-grace id strands the player at (0,0,0); id-validation not yet feasible

**Status:** open (low severity — recoverable). Documented 2026-07-05 (Linux/Proton, ERRv2.2.9.6).

## What happens
`warp <id>` (`goblin::warp::to_grace` → `LuaWarp_01`) with an id that is **not a real/unlocked grace**
does NOT cleanly no-op — the game teleports the player to **`local=(0,0,0)`, map-pos unresolved** (a
broken/void cell). Ground-truthed live: `warp 1044600000 / 1048572370 / 1037500100 / 1042617188` all land
at `(0,0,0)`; `warp 1042362951` (First Step) and `warp 1034509302` land at real spots. **Recoverable** — a
subsequent VALID warp fixes the position (no hard soft-lock). `to_grace` returns `ok` regardless because
`LuaWarp_01` "was called and didn't fault"; ok ≠ "destination valid". The load watchdog only catches a HUNG
warp, not a to-origin one.

## Why id-validation was NOT shipped (the interesting part)
Tried to gate bad ids by checking the target against the live `BonfireWarpParam` (`goblin::live_graces()`).
It **wrongly rejected valid graces** and a live diagnostic showed why:

- `[WARP][IDDIAG] code=1042362951 graces=438 matchByEntityId=false matchByRowId=false samples:
  [row=100000 ent=10001950] [row=100001 ent=10001951] …`

The valid, working First-Step warp id **1042362951 matches NEITHER** the captured `rowId` (6-digit
`100000…`) **nor** `bonfireEntityId` (8-digit `10001950…`) of ANY of the 438 captured graces. So the
captured param data does not expose the value `LuaWarp_01` actually accepts → it can't be a whitelist yet.

⚠ This **contradicts** [[vmap-grace-warp-entity-id]], which states the warp id IS
`BonfireWarpParam.bonfireEntityId @0x08` (e.g. `1042362951` = First Step). Live, the captured
`bonfireEntityId` field reads as `10001950`-style values instead. Either (a) `capture_live_graces` reads
`bonfireEntityId` from the wrong offset (the value it stores is some other field), or (b) the working warp
id is a computed/remapped value not equal to any raw param field, or (c) First Step's row is filtered out
by the capture's `dispMask0 & 0x7 == 0` / flag / name skips. **Resolving which is the prerequisite** for any
warp-id validation — RE the true warp-id↔`BonfireWarpParam` field (and reconcile with the resolved note).

## Guardrail
- `warp` "ok" means "LuaWarp fired", NOT "landed somewhere valid". Only pass ids known to be real unlocked
  graces (`1042362951` First Step is the safe default). For automated multi-warp sweeps, read `coords` after
  each warp and treat `(0,0,0)` / map-unresolved as a bad id.
- Don't gate warp ids against `live_graces()` until the field mystery above is resolved — it false-negatives
  on valid ids (First Step included).
