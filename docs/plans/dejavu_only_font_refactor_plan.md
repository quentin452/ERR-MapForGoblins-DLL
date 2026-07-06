# Plan: single-DejaVu overlay font (drop the ProggyClean + DejaVu merge)

**Status:** IMPLEMENTED 2026-07-06 (`src/goblin_overlay.cpp` font block). Single DLL builds clean.
Size = 15px (live-tuning knob, not yet verified in-world via screenshot). Needs game restart to load.
**Goal:** one rasterizer, one baseline for the whole overlay. Today ASCII is a ProggyClean
**bitmap** and accents/extended glyphs are a **DejaVu TTF** grafted on by merge. The two paths
differ in hinting, oversampling, and vertical placement, so accented chars read raised/blurry
next to the crisp pixel ASCII. Loading DejaVu as the SINGLE primary font makes every glyph share
the same rasterizer + baseline → the mismatch cannot exist.

---

## 1. Current state (exact, `src/goblin_overlay.cpp` ~1235-1297)

Two fonts, ASCII split off from extended and merged back:

```cpp
static const ImWchar kAsciiRange[] = { 0x0020, 0x007E, 0 };
ImFontConfig baseCfg;
baseCfg.OversampleH = baseCfg.OversampleV = 1;
baseCfg.PixelSnapH = true;
baseCfg.GlyphRanges = kAsciiRange;
io.Fonts->AddFontDefault(&baseCfg);        // ProggyClean, ASCII 0x20-0x7E ONLY

static const ImWchar kExtRanges[] = {
    0x00A0, 0x00FF,  // Latin-1 Supplement (accents)
    0x0100, 0x024F,  // Latin Extended-A + B (œ, more accents)
    0x0370, 0x03FF,  // Greek
    0x0400, 0x04FF,  // Cyrillic
    0x2010, 0x2027,  // general punctuation (en/em dash, curly quotes, …)
    0x2190, 0x21FF,  // arrows (→)
    0x2200, 0x22FF,  // math (≤ ≥ ≠ × ÷)
    0x20A0, 0x20BF,  // currency
    0,
};
ImFontConfig cfg;
cfg.MergeMode = true;
cfg.PixelSnapH = true;
cfg.GlyphOffset.y = 1.0f;   // hand-matched to ProggyClean's baseline at 13px
cfg.OversampleH = 2;
cfg.OversampleV = 1;
io.Fonts->AddFontFromMemoryCompressedTTF(
    DejaVuSans_compressed_data, DejaVuSans_compressed_size, 13.0f, &cfg, kExtRanges);
```

Key facts:
- ASCII owner = ImGui's built-in **ProggyClean** bitmap at 13px (`AddFontDefault`).
- Extended owner = embedded **DejaVu Sans** TTF, merged at 13px, `GlyphOffset.y = 1.0`,
  `OversampleH = 2`.
- The `GlyphOffset.y = 1.0` is a manual fudge (commit 7971908) to drop DejaVu accents down onto
  ProggyClean's baseline — proof the two baselines don't natively agree.
- Embedded source: `src/generated_shared/dejavu_sans_ttf.h` →
  `DejaVuSans_compressed_data` + `DejaVuSans_compressed_size` (= 495016). Same array already used,
  so **no new asset** — this refactor is init-code-only.
- System-font (segoeui/arial/tahoma) merge stays only as a decompress-failure fallback.

---

## 2. Proposed change (the new init block)

Replace the whole block with ONE non-merge DejaVu font that owns ASCII **and** extended:

```cpp
ImGuiIO &io = ImGui::GetIO();

static const ImWchar kFontRanges[] = {
    0x0020, 0x00FF,  // ASCII + Latin-1 Supplement (accents) — one contiguous block
    0x0100, 0x024F,  // Latin Extended-A + B
    0x0370, 0x03FF,  // Greek
    0x0400, 0x04FF,  // Cyrillic
    0x2010, 0x2027,  // general punctuation
    0x2190, 0x21FF,  // arrows
    0x2200, 0x22FF,  // math operators
    0x20A0, 0x20BF,  // currency
    0,
};
ImFontConfig cfg;
// NO MergeMode, NO AddFontDefault, NO GlyphOffset fudge — single primary font.
cfg.OversampleH = 2;          // sharpen the antialiased TTF at small px
cfg.OversampleV = 1;
cfg.PixelSnapH  = true;       // keep glyphs on the pixel grid → crisper
const float kFontPx = 15.0f;  // NEEDS LIVE TUNING — see below
if (!io.Fonts->AddFontFromMemoryCompressedTTF(
        DejaVuSans_compressed_data, DejaVuSans_compressed_size, kFontPx, &cfg, kFontRanges))
{
    // decompress failed → keep ImGui's built-in ProggyClean so the UI still draws
    io.Fonts->AddFontDefault();
    // (optional) system-font fallback loop, as today
}
```

Deltas vs current:
- Delete `AddFontDefault(&baseCfg)`, `kAsciiRange`, `MergeMode`, `GlyphOffset.y = 1.0`.
- `kExtRanges` becomes `kFontRanges` with its first block widened `0x00A0→0x0020` so DejaVu covers
  ASCII too. Everything else identical.
- One rasterizer ⇒ baseline is intrinsically consistent; the offset fudge is no longer needed.

### Sizing (the one real unknown)

DejaVu Sans has a **smaller x-height per em** than ProggyClean, so DejaVu at 13px *reads smaller*
than ProggyClean at 13px. To match perceived readability, bump the pixel size:
- Start at **15px** (`kFontPx = 15.0f`); 14px is the likely floor if 15 feels large.
- **Flag: this is a live-tuning knob, not a derivable constant.** Screenshot at 15, compare letter
  height to the old ProggyClean build, adjust ±1px. Pick the value where lowercase body text has
  roughly the same cap/x-height footprint as before.
- Keep `OversampleH = 2` + `PixelSnapH = true` for edge crispness at this small size (subpixel
  positioning is the main softness source for small TTF text). `OversampleV = 1` is fine — vertical
  oversampling rarely helps and costs atlas area.

---

## 3. Risk / impact

This swaps the font for the **entire overlay** — F1 panel, vmap labels, minimap labels, enemy
names, tooltips, region labels — because they all draw with the single shared ImGui font. Larger
per-glyph metrics ripple into every place that measures or positions text.

### Font-metric-dependent call sites (grep results — audit + re-tune)

All of these read `GetFontSize()` / `CalcTextSize*`, so they auto-scale with the new size (good),
but the *multipliers/offsets* around them were eyeballed against 13px ProggyClean and may want a nudge:

| File:line | Code | Impact |
|-----------|------|--------|
| `src/worldmap/map_renderer.cpp:1797` | `fontSize = GetFontSize() * 1.6f * uiScale` (region/area labels) | Scales up automatically; labels get bigger. Re-check the `*1.6` feels right; may drop toward ~1.4. |
| `src/worldmap/map_renderer.cpp:1815,1839,1840` | `CalcTextSizeA(fontSize,…)` + shadow at +1.5px | Auto-tracks; the fixed **1.5px** shadow offset is now relatively smaller vs a bigger glyph — cosmetic, low risk. |
| `src/overlay_panel/panel_virtual_map.cpp:2119` | `fontSize = GetFontSize() * 1.4f * uiScale` (enemy/entity names) | Auto-scales; verify names don't overlap after the bump. |
| `src/overlay_panel/panel_virtual_map.cpp:2130,2147,2148` | `CalcTextSizeA` + `1.5f*uiScale` shadow | Auto-tracks; cosmetic shadow only. |
| `src/overlay_panel/panel_virtual_map.cpp:1396` | `GetFont()->CalcTextSizeA(GetFontSize(),…)` centered message | Auto-centers; fine. |
| `src/overlay_panel/panel_virtual_map.cpp:2050` | `r = (13.0f + pulse*10)*uiScale` | **NOT a font metric** — it's a pulse-ring radius that happens to use `13.0f`. Leave it; do not "fix" it to track the font. |
| `src/goblin_overlay_render.cpp:216,225` | `GetFontSize()*tscale` + `CalcTextSize` (marker labels) | Auto-scales; verify marker text legibility. |

Net: no site hardcodes ProggyClean glyph metrics or a 13px text *height*; all use the runtime
`GetFontSize()`, so nothing breaks structurally. The work is **re-tuning multipliers** (the `*1.6`,
`*1.4`) and eyeballing label overlap, not fixing broken layout.

The only literal `13.0f` in a font-shaped spot (`panel_virtual_map.cpp:2050`) is a radius, not text —
leave it.

### Atlas size

Roughly neutral, likely **smaller**. Today the atlas bakes ProggyClean's full bitmap **plus** a
partial DejaVu (all of `kExtRanges`, ~1000+ glyphs, oversampled 2×). The new build bakes DejaVu once
over ASCII+extended (ASCII adds ~95 glyphs to a set DejaVu already rasterizes). One rasterizer + one
oversample setting ⇒ no duplicate ASCII coverage. Bump to 15px slightly enlarges each glyph cell but
dropping ProggyClean's whole bitmap sheet offsets it. Expect parity or a small win; not a concern.

### Host-side / hot-reload note

The font atlas is built **once at overlay init** (`goblin_overlay.cpp`, host side, before
`ImGui_ImplDX12_Init`). It is NOT in the swappable render DLL, so the overlay hot-reload path does
**not** pick up a font change — **implementation later needs a full game restart** to rebuild the
atlas. (No host↔render boundary call is added/moved, so the `build-linux-hotreload` link check is not
triggered by this change.)

---

## 4. Verification (post-change)

Via the debug RPC `screenshot` verb, in-world (see `docs/memory/tooling/rpc-commands.md`):
1. **F1 panel** open — confirm body text is crisp and readable at the chosen px; compare against a
   pre-change screenshot for size parity.
2. **Accented item name** — warp/loot something like "White Mask Varré" (é), an œ name, an em-dash
   label. Confirm the accent sits ON the letter's baseline (no raised/floating accent) and the
   accented glyph matches the weight/sharpness of the surrounding ASCII — the whole point of the
   refactor.
3. **Enemy name** (`panel_virtual_map` path) + a **region/area label** (`map_renderer`) — confirm no
   overlap/clipping after the size bump; re-tune `*1.6` / `*1.4` if labels feel big.
4. **Scale sweep** — check at 1080p and 4K via the existing `uiScale` knob; confirm the font tracks
   `uiScale` at both (all sites already multiply by `uiScale`).
5. Grep-diff the changelog/memory: if shipped, add an `[Unreleased]` line + a `docs/memory/bugs/`
   note that the raised-accent artifact is resolved.

---

## 5. Decision framing

**Option A — ship all-DejaVu (this plan).** One rasterizer, one baseline, guaranteed consistency;
the raised/blurry-accent class is *structurally impossible*, not fudged. Cost: loses ProggyClean's
distinctive pixel look; every ASCII glyph is now antialiased TTF (softer at small px, mitigated by
oversample + PixelSnapH + the size bump). Simpler init, one fewer font, one fewer manual offset to
rot. Mod-agnostic: unaffected (embedded font, same on Wine/Proton).

**Option B — keep the aligned split (commit 7971908).** Retains crisp pixel ASCII; accents already
baseline-matched via `GlyphOffset.y = 1.0`. Cost: two rasterizers will always differ subtly in
hinting/weight, the offset is a hand-tuned constant that silently rots if the base size changes, and
"crisp bitmap next to soft TTF" is inherently visible on close inspection.

**Recommendation: Option A (all-DejaVu).** The split's alignment is a fragile per-size fudge and can
never fully hide the bitmap-vs-TTF texture difference; a single font removes the whole failure class
for a one-time readability re-tune (pick ~15px, nudge two multipliers). The pixel-font aesthetic is a
minor loss against a uniform, self-consistent overlay. Land it, tune size live, keep Option B's
`GlyphOffset` trick documented as the fallback if all-DejaVu reads too soft.
