---
name: imgui-unicode-font
description: "ImGui overlay font: default is Latin-1 only (œ/CJK → '?'); fix = merge a system font for extended glyphs. TODO: bundle a TTF in the DLL instead of relying on C:\\Windows\\Fonts."
metadata: 
  node_type: memory
  type: project
---

**ImGui overlay glyph coverage (commit 818bac4).** The overlay's marker labels/tooltips/F1 text are
already correct UTF-8 (`goblin::lookup_text_utf8` → `WideCharToMultiByte(CP_UTF8)`), but ImGui used its
DEFAULT font (ProggyClean) which only has **Latin-1 glyphs (0x20–0xFF)** → any char beyond renders as
ImGui's **FallbackChar '?'** (e.g. French "Cœur" → "C?ur" because œ = U+0153; also em-dash, Cyrillic,
Greek). It is a GLYPH-COVERAGE issue, NOT encoding.

**Fix (goblin_overlay.cpp, right after `ImGui::CreateContext()`, before `ImGui_ImplDX12_Init`):** keep
`AddFontDefault()` for the ASCII look, then **MergeMode-merge** a broad Windows system font
(segoeui.ttf → arial.ttf → tahoma.ttf, first that exists) for extended ranges: Latin Ext-A/B (0x100-
0x24F), Greek, Cyrillic, General Punctuation (0x2010-0x2027), currency. Falls back to default if none
found. This earlier forced ASCII workarounds (e.g. F1 header em-dash → '-') which can now be reverted
to real Unicode.

**✅ PERF PASS 1 (2026-06-24, commit 8b7560d, UNPUSHED) — render bench + the dominant label fix.**
ROOT cause of the lag at 1000+ markers: `render_markers` (map_renderer.cpp) built `marker_label(m)`
for EVERY on-screen marker every frame just to pass it to `hover_test`, which keeps only the hovered
one. `marker_label` = 2× `lookup_text_utf8` = FMG lookup + 2 WideCharToMultiByte syscalls + a heap
std::string EACH → N string-builds/frame for ONE tooltip. FIX: `hover_test` is now a template taking a
`make_label` thunk, invoked ONLY when the point passes the cheap squared-distance test AND is a new
closest-within-radius candidate → ~N builds collapse to the handful near the cursor. Behaviour
identical (empty labels still skipped). BENCH: split the existing single `render.worldmap` quiet timer
into `render.worldmap.markers` (main project+cull+gate+draw loop) + `render.worldmap.clusters` (pile
pass) in the [BENCH] session report, + a throttled `[RENDERPERF]` count log (gated debug_logging) of
markers iterated / drawn / deferred. NOTE the loop ALREADY culls off-screen before the gates (project
cached after prewarm) and clusters dense areas — those weren't the waste.
**✅ VALIDATED 2026-06-25 — render is NOT the bottleneck; #3 CLOSED.** Runtime stress test (all 7
sections ON, loot_collectibles ON, clustering OFF = absolute worst case): `[RENDERPERF] iterated=10170
drawn=4865 deferred=0 group=0`; `render.worldmap.markers` avg **1.48 ms**, max 2.69 ms over 962 frames
(`render.worldmap` avg 1.67, the lone max 15.65 = the one-time prewarm frame, not steady). At 4865
drawn markers the loop is 1.48 ms — i.e. the deferred-label fix removed the ~5-10 ms/frame of FMG/UTF8
label builds that WAS the felt lag. <user>'s box = GTX 1650 Ti runs ERR at ~50 fps NORMALLY (GPU-bound,
20 ms/frame budget); our render is ~7% of that, not the cause. **Pass 2 NOT needed**: the 10170 raw
iterations are near-free (int compare + cached projection + cull); the only remaining cost is the 4865
intended AddImage draws — reduce them via clustering (enable_clustering=true) or category hiding (#2),
not code. NO per-frame draw cap (would hide loot). OPTIONAL polish only: amortize the one-time prewarm
(15 ms single frame on map-open) over a few frames. census path never needed benching (report was clean).

**✅ DONE 2026-06-25 (commit e16ab63, UNPUSHED) — TTF embedded in the DLL (no more C:\\Windows\\Fonts).**
User chose European-only DejaVu Sans. Method: ImGui's `binary_to_compressed_c` (compiled host-side with
clang-cl + the xwin SDK include/lib paths — plain `clang-cl file.cpp` fails, no native MSVC; pass
`-imsvc D:\mfg_toolchain\xwin-sdk\{crt\include,sdk\include\ucrt,um,shared}` + matching libpaths) →
`src/generated_shared/dejavu_sans_ttf.h` (compressed_size 495016, ~483 KB; 1.5 MB source). Wired in
goblin_overlay.cpp via `AddFontFromMemoryCompressedTTF(DejaVuSans_compressed_data, _size, 13.0f, &cfg,
kExtRanges)` over the SAME ranges (Latin-1/Ext-A-B/Greek/Cyrillic/punct/currency). The old
segoeui/arial/tahoma merge stays ONLY as a last-ditch fallback if the embedded decompress fails. DLL
4.96→5.44 MB. Headers need no CMakeLists entry (only .cpp). CJK/Thai/Arabic deliberately NOT covered
(would need a dedicated font + larger atlas; user is French, European suffices) — if ever needed, the
clean approach is a runtime system/game-font fallback for those scripts, NOT a 16 MB embed. DejaVu =
Bitstream Vera / Arev license (redistributable, embeddable, no OFL reserved-name constraint). See
[[aeg-collectible-source]] (the Runic Trace label that exposed the original glyph gap).
