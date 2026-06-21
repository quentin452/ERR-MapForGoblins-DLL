# Cheat Engine plan — find the underground-correct player position (yellow-dot source)

Runtime recipe for `windows_player_pos_runtime_ct_re_prompt.md` (commit 6a68c1e), after static RE
peeled only render-layer functions. App 2.6.2.0 / ERR 2.2.9.6, imagebase `0x140000000`. Ground truth
= the native yellow "you are here" dot (correct on every page). Goal: a value that is **non-origin
and tracks movement underground**, then a stable AOB-anchored pointer chain.

Reference readout you already have: the **er-console-mod** coord display (block-local) — use it to
sanity-check what CE finds.

---

## Phase 0 — quick-test the chains I already derived (5 min, may end it here)

Add these as CE pointer entries (type **Float**), stand at **Ancestral Woods / Siofra**, and watch
while moving. `ER` = the `eldenring.exe` module base.

**Chain A — player physics Vec (corrected leaf):**
```
base = [ER + 0x3D65F88]            ; WorldChrMan (AOB WCM_FINDER below)
      [base + 0x1E508]             ; LocalPlayer
      [.. + 0x58] [.. + 0x10] [.. + 0x190] [.. + 0x68]
X = float @ +0x70 , Y(height) = +0x74 , Z = +0x78
```
(CE offset list, outer→inner: `0x70 / 0x68 / 0x190 / 0x10 / 0x58 / 0x1E508`, base `ER+0x3D65F88`.)

**Chain B — map-point manager mirror:**
```
[ER + 0x3D69BA8] + 0x70 (X) / +0x74 (Y) / +0x78 (Z)
```

**Decide:** move N/S/E/W → exactly TWO floats should change = the horizontal axes (expect `+0x70`,
`+0x78`); jump / slope → the THIRD changes = height (`+0x74`). 
- If A or B's `+0x70`/`+0x78` **track horizontal movement underground** and sit near the real spot →
  the source is found (it's sub-area-local). Skip to **Phase 3** (frame + chain). 
- If both read static/origin and don't move → the source is elsewhere → **Phase 1**.

> Phase 0 also settles the open question cheaply: is the manager value real-but-wrong-frame
> (tracks movement) or genuinely dead underground (doesn't move)?

---

## Phase 1 — value-scan the player coords (if Phase 0 fails)

The player's world position is a live float that moves with you — the classic CE find.
1. CE → Scan Type **Float**, Value Type, **Unknown initial value**, First Scan.
2. Walk **north** a few metres → **Increased value** (then **Decreased value** after walking south).
   Strafe E/W and repeat for the other axis. 4–6 scans isolates the X and Z coordinate addresses.
   (Jump to isolate Y/height if you want all three.)
3. Underground values are **small** (sub-area-local) — that's expected, not a failure. If Float finds
   nothing stable, retry as **Double** (some ER transforms are doubles).
4. Keep 2–3 green addresses that move smoothly and stop when you stop.

## Phase 2 — confirm + get a stable pointer chain

1. Right-click the X address → **Find out what accesses this address**. Walk around (map CLOSED, for
   the minimap; also OPEN) → the accessing instructions' base register points at the player position
   struct. Note the struct base; cross-check it against Chain A's nodes (is it off WorldChrMan
   `0x3D65F88`, or a sub-area/world-block manager?).
2. Right-click → **Pointer scan for this address**. Save. **Reload at a different spot / restart the
   game** → **Rescan → filter by the new address**. Do 2–3 restarts → a handful of stable paths from
   `eldenring.exe+<static>`. Pick the shortest stable one.
3. Re-derive the static base by **AOB** (RVAs drift per patch) — see below.

## Phase 3 — frame + validation (the part that actually matters)

The projection is already correct (graces drawn via `marker_world_pos(area,gx,gz,posX,posZ,
conv_underground=true)` land right). So determine **which frame** the found value is in:
- **(a) already map/marker space** (same units as `WorldMapPointParam.posX/posZ`) → feed it straight
  into the existing projection. *This is the ideal target — prefer hunting the value that's already
  here.*
- **(b) sub-area-local / world** → needs world→map conversion. The MapId tile (`+0x2c`) is a coarse
  block id underground, NOT the marker grid — that mismatch is the current bug. Avoid reconstructing
  from the tile; instead value-scan the dot's **marker-space** position directly (it is pan/zoom
  independent), or read it from the player-marker object.

**Validate on every page**, comparing to the yellow dot: overworld, base underground **m12** (≥2
spots — Ancestral Woods + a Nokron/Ainsel grace), DLC **m40–m43** if reachable. Correct = non-origin,
tracks movement, lands on the dot.

> Authoritative alt-target if value-scan is noisy: in CE, **Find out what accesses** the yellow
> dot's on-screen position while the map is open; the writing instruction derefs the true source one
> step before the GFx fit (`FUN_140d84990`, stage→screen) — back up from there to the marker-space
> player pos.

---

## Deliverable back to the mod

1. **.CT** with the working entry(ies) as float X/Z (+ area/tile if a bridge is needed), AOB/pointer
   anchored (survives restart). Commit under `docs/re/`.
2. **Recipe**: the anchoring **AOB** (+ match count, `relative_offsets`), full **deref chain + float
   offsets**, the **frame** (a/b above), and confirmation it updates **map-CLOSED** (minimap) and the
   SAME source works overworld (one code path).

## AOBs to re-derive the static bases (don't ship RVAs)

- WorldChrMan `ER+0x3D65F88` — `WCM_FINDER` = `48 8B FA 0F 11 41 70 48 8B 05` (slot = `mov rax,[rip]`
  at +7; rel `{{0xA,0xE}}`). Already PASS in `re_signatures.hpp`.
- Map-point/geom manager `ER+0x3D69BA8` — `WORLD_GEOM_MAN_SLOT`
  `48 8B 0D ?? ?? ?? ?? 48 8D 53 10 E8 ?? ?? ?? ?? 4C 8B E8` (rel `{{3,7}}`). Load site also at
  `FUN_140623410` `140623d94` (`MOV RCX,[0x143d69ba8]`).
- MapId singleton `ER+0x3D691D8` — `PLAYER_MAPID_SLOT` (rel `{{3,7}}`); packed id `+0x2c`
  (area`>>24`, gridX`>>16`, gridZ`>>8`).

## What static already established (so you don't re-derive)
- Vec layout at the chain leaf / manager: **X@+0x70, Y@+0x74 (height), Z@+0x78** (`FUN_14045e390`
  copies `+0x70..+0x7f`). The mod's `+0x74`-as-Z is wrong; Z is `+0x78`.
- `FUN_140d82770`→`FUN_140d84990` is the **GFx stage→screen fit** (reads active render dims
  `DAT_1447ef360+0x128+0x118`), NOT a world→map projection. `cursor+0x90` is a Scaleform
  `SceneObjProxy`. So the cursor render path is not the world→map source.
- Overworld works because MapId tile == the WorldMapPointParam 60-grid there; underground it does not.
