# RE findings — grace pin DRAW vs TELEPORT (they share the GFx widget; the only draw-only lever is the icon image)

Answers `docs/re/windows_grace_warppin_teleport_re_prompt.md`. Static Ghidra RE
(`D:\ghidra_proj2\ER`, scripts `re_v122..v124`). App 2.6.2.0 / ERR 2.2.9.6, imagebase
`0x140000000`. Read-only.

---

## 0. TL;DR — why `+0x60` killed teleport, and the one clean draw-only lever

Each grace pin is rendered as a **GFx (Scaleform) widget**. `vt[1]
WorldMapWarpPinData::SetTo = FUN_14087ae20` binds that widget every refresh and sets the
**widget `_visible` directly from the cached visible byte `pin+0xC`**
(`FUN_140733340(widget, *(byte*)(pin+0xC))` → `FUN_140d844d0` = set `_visible`). `pin+0xC`
is recomputed each tick by `vt[3] FUN_14087afa0` from the state bitmask `pin+0x60`.

**The map cursor only snaps to / sends click input to a `_visible` widget.** So zeroing
`pin+0x60` → `pin+0xC`=0 → `widget._visible=false` → the pin is no longer hoverable →
**no fast-travel.** The teleport does *not* read `pin+0x60` itself; it is gated
**indirectly** through widget visibility. Draw and click are therefore **coupled at the
`_visible` flag** — there is **no separate per-pin "render" vs "hit" flag** to flip.

**The only draw property independent of `_visible` is the ICON IMAGE.** `SetTo` also binds a
child sub-widget `"Icon_0"`/`"IconImage"` via `FUN_14074bcc0(iconChild, vt12(pin))`. If the
icon descriptor is invalid (frame `<1` or zero-size rect), `FUN_14074bcc0` hides **only the
`IconImage` child**, leaving the **outer pin widget `_visible` (still cursor-selectable)**.

**Recommended draw-only recipe (keeps teleport):** hook `vt[1] SetTo FUN_14087ae20`; after
the original runs, for a discovered `WorldMapWarpPinData`, **re-hide just its `Icon_0`
child** (`FUN_140733340(Icon_0, 0)`), leaving `pin+0xC` / `pin+0x60` untouched. Icon gone,
pin still warp-clickable. **Do NOT field-poke the pin** — see §2 (the icon sub-structs are
re-filled at every SetTo, the same layer-trap that made the `+0xC` and now the `+0x60`
writes fail).

> The shipped `+0x60` write (commit d8d2125) is **superseded** — it suppresses the draw but
> breaks teleport. `grace_suppress_native` already defaults **false**, so teleport is safe as
> shipped; leave it off until the `SetTo` recipe lands.

---

## 1. Q1 — the teleport path and what it reads

- The grace pin draws + receives input as a **single GFx widget** (one per pin). `vt[1]
  SetTo FUN_14087ae20` sets `widget._visible = pin+0xC` (`FUN_140733340` →
  `FUN_140d844d0`). No `_visible` → no cursor snap → no confirm → no warp.
- The warp itself is keyed off the pin's own warp identity, **not** `pin+0x60`:
  - `pin+0x54` = warp **id** (`[srcWarpData+0x18]`), checked in SetTo via `FUN_1405d1330`
    (the `"CS::WorldMapPinData::SetTo"` reg lookup) to drive the `"Cleared"` widget state.
  - `pin+0x240` = the warp **sub-record** (`id @+0x4`, zoom thresholds `@+0x12/+0x13`),
    read by `vt[13] IsOpen FUN_14088bd70` (consults the dev-override reg `DAT_143d68448`
    with `"CS::WorldMapWarpPinData::IsOpen"` — `FastTravelOverrideReg` is its UTF-16 key).
  - source `srcWarpData = *(pin's WarpData+0x8)`: bonfire/grace id `@+0x18`, iconId `@+0x08`,
    state `@+0x1E`.
- **So: teleport needs the widget `_visible` (gated by `pin+0xC` ← `pin+0x60`), then warps by
  `pin+0x54`/`pin+0x240`.** That is exactly why the `+0x60` suppression took fast-travel down
  with the icon.

## 2. Q2 — is there a draw-only field? Only the icon image (and it can't be field-poked)

No per-pin flag separates render from hit — both ride `widget._visible`. The lone separable
draw element is the **`Icon_0`/`IconImage` child**:

`SetTo` → `FUN_14074bcc0(iconChild, desc)` where `desc = vt12(pin)` (`vt[12] FUN_14088bb60`
returns one of four per-layer icon sub-structs `pin+0x248 / +0x288 / +0x2c8 / +0x308`,
selected by a `pin+0x238` map check and the `pin+0x348` flag). `FUN_14074bcc0` reads the
descriptor as `int* d`:

| desc field | off | meaning |
|---|---|---|
| `d[0]` | +0x00 | frame index (`"Frame%d"`) |
| `d[1..4]` | +0x04..+0x10 | rect (x0,y0,x1,y1) |
| `d[0xe]` | +0x38 | texture name/handle |
| `d[0xf]` | +0x3c | mode (`<0` = frame/rect branch; builder seeds it `-1`) |

In the frame branch, **`if (d[0]<1 || width<1 || height<1)` → it hides only the `IconImage`
child** (`FUN_14074bb10`→`FUN_140733340(iconChild,0)`) and the outer pin stays visible.

**Why you can't just poke the pin:** these sub-structs hold only *seeds* at build time (the
builder writes zeros + `mode=-1`); the real frame/rect/texture is filled at **every `SetTo`
/ refresh**. A `WriteProcessMemory` to `pin+0x248…` (or to `pin+0x50` iconId) is overwritten
on the next refresh — the identical "wrong layer" failure as zeroing the cached `pin+0xC`.
The icon must be suppressed **at the apply point (the `SetTo` virtual)**, not on the object.

## 3. Q3 — the global pin/callback registry (draw vs interaction)

```
WorldMapViewModel  (ctor FUN_1408855b0 @0x8855b0)
 ├─ warp pin list   VM+0x2d8   WorldMapPinDataList<WorldMapWarpPinData>  (vtable 0x2ad82a8,
 │                              stride 0x350; count vt FUN_140889610, at vt FUN_140886530)
 │     each elem = WorldMapWarpPinData (vtable 0x2ad8228) — graces/bonfires
 ├─ (sibling pin lists per kind: WorldMapPointPinData / SignPuddle / WorldMapPinData)
 └─ map-tile / legacy-conv icon structs (FUN_140876100 + BCD icon id FUN_140660d20)
```

**One container, one widget per pin** — the renderer AND the cursor/teleport walk the **same**
VM warp list and the **same** GFx widgets. There is no separate "draw list" vs "callback
list" to split. Per-entry, the fields divide as:

- **DRAW:** `vt[1] SetTo FUN_14087ae20` → `widget._visible = pin+0xC` (← `vt[3] FUN_14087afa0`
  ← state `pin+0x60`, per-layer bit) **+** `Icon_0` image (← per-layer sub-structs
  `pin+0x248/+0x288/+0x2c8/+0x308` via `vt[12]`+`FUN_14074bcc0`).
- **INTERACTION:** the same widget must be `_visible` to be hoverable; warp executes from
  `pin+0x54` (id) / `pin+0x240` (sub-record) / source WarpData. Hover/tooltip = same widget.

**Generalisation:** to suppress ANY pin type's draw while keeping its interaction, the lever is
always **the `Icon_0`/`IconImage` child of its GFx widget** (hooked at that type's `SetTo`
virtual), never the `_visible`/state fields. Point pins share `vt[1] FUN_14087ae20` and
`vt[3] FUN_14087afa0` with warps, so the same hook (guarded by the object vtable at `pin+0x0`)
covers them.

## 4. Q4 — the recipe to implement (exact handles)

Hook **`FUN_14087ae20`** (`er+0x87ae20`, `vt[1]` of both Warp & Point pins). Signature
`longlong* SetTo(longlong* pin, longlong widgetRoot, undefined8, undefined8)`.

```
ret = orig(pin, widgetRoot, a3, a4);
if (graceOverlay && graceSuppressNative
    && *(void**)pin == WorldMapWarpPinData::vftable   // grace only (resolve vtable by AOB)
    && discovered(pin))                               // (*(u32*)(pin+0x60) & 7) != 0
{
    // fetch the Icon_0 child of widgetRoot and hide ONLY it; leave the outer pin _visible
    GFxProxy child; FUN_14074a2f0(widgetRoot, &child, "Icon_0");   // 0x74a2f0
    FUN_140733340(&child, 0);                                      // 0x733340 -> set _visible 0
    FUN_140d7f850(&child);                                         // release proxy (as vanilla)
}
return ret;
```

⚠ `FUN_14074a2f0` returns a stack proxy that vanilla code releases with `FUN_140d7f850` — the
proxy struct layout must match (see `SetTo`'s `local_78[40]`/`local_50[56]`). Resolve all fns
+ the `WorldMapWarpPinData::vftable` by AOB (ASLR). Runtime-test for leaks/crashes; if the
proxy handling is risky, the safe fallback is the **hybrid** default (below).

**Safe interim (already in place):** `grace_suppress_native` defaults `false` → native draws +
teleports discovered graces, overlay covers undiscovered. Keep this until the `SetTo` hook is
verified in-game.

## 5. Handles / RVAs (resolve by AOB; ASLR)
- `vt[1] SetTo` `FUN_14087ae20` `0x87ae20` (binds widget; `widget._visible = pin+0xC`; icon via `vt[12]`).
- GFx set-`_visible` `FUN_140733340` `0x733340` (→ `FUN_140d844d0` `0xd844d0`).
- GFx get-child `FUN_14074a2f0` `0x74a2f0`; proxy release `FUN_140d7f850` `0xd7f850`.
- GFx icon-image setter `FUN_14074bcc0` `0x74bcc0` (desc `int*`: `[0]`frame `[1..4]`rect `[0xe]`tex `[0xf]`mode).
- icon descriptor selector `vt[12] FUN_14088bb60` `0x88bb60` → `pin+0x248/+0x288/+0x2c8/+0x308`.
- visible calc `vt[3] FUN_14087afa0` `0x87afa0` (`pin+0xC` ← `pin+0x60`); IsOpen `vt[13] FUN_14088bd70` `0x88bd70`.
- warp id `pin+0x54`; warp sub-record `pin+0x240`; state bitmask `pin+0x60`; cached visible `pin+0xC`; iconId `pin+0x50`.
- container: VM warp list `VM+0x2d8` (vtable `0x2ad82a8`, stride `0x350`), VM ctor `FUN_1408855b0` `0x8855b0`.
- dev-override reg singleton `DAT_143d68448` (`FastTravelOverrideReg`, utf16).
```
