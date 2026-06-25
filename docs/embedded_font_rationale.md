# Why the overlay embeds a TTF (instead of reading a system font)

_Decision record — 2026-06-25. Implemented in commit `e16ab63`
(`src/goblin_overlay.cpp`, `src/generated_shared/dejavu_sans_ttf.h`)._

## Problem

ImGui's default font (ProggyClean) only carries Latin-1 glyphs (`0x20`–`0xFF`).
Any character beyond that — the French **œ** ligature (U+0153, Latin Extended-A),
em-dash (U+2014), curly quotes, Greek, Cyrillic, the € sign (U+20AC) — renders as
ImGui's fallback `'?'`. The overlay's labels/tooltips are already correct UTF-8
(`goblin::lookup_text_utf8` → `WideCharToMultiByte(CP_UTF8)`), so this is purely a
**glyph-coverage** gap, not an encoding bug. It first surfaced as the ERR "Runic
Trace" / "Cœur…" item names rendering as "C?ur".

The original fix merged a **Windows system font** at startup
(`C:\Windows\Fonts\segoeui.ttf` → `arial.ttf` → `tahoma.ttf`, first that exists)
for the extended ranges. That works on a normal Windows install but is
**machine-dependent**, and the mod also runs under Proton/Wine on Linux and Mac.

## Why not just probe the system font paths per-OS?

Tempting — "we roughly know where TTFs live on Windows, Linux/Arch, and Mac". The
reason we **don't** is that Elden Ring is a Windows game: on Linux/Mac it runs via
**Proton/Wine**, so our DLL is **always a Windows PE** and sees the **Wine
filesystem**, not the native one. Concretely, probing system fonts fails or
fragments:

1. **Wine doesn't guarantee real `.ttf` files at the Windows paths.** Wine
   substitutes fonts at the GDI API level (it answers "give me Arial" with a Linux
   font) but does **not** necessarily place `C:\windows\Fonts\arial.ttf` as a real
   file. `AddFontFromFileTTF` reads a file by path, so the substitution doesn't help
   us — the file may simply be absent.
2. **Native Linux paths fragment by distro.** Reachable via Wine's `Z:` → `/`
   mapping, but the location varies:
   - Arch / SteamOS: `/usr/share/fonts/TTF/DejaVuSans.ttf`
   - Debian/Ubuntu: `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`
   - Fedora: `/usr/share/fonts/dejavu-sans-fonts/`
   A hardcoded path list is a guess that rots across distros and releases.
3. **The font may not be installed at all** — a lean Proton prefix, a minimal Steam
   Deck image, or a headless setup can lack any given family, reintroducing the `'?'`
   bug we set out to fix.
4. **`fontconfig`** (the correct Linux font resolver) is effectively unreachable from
   a Windows PE under Wine.
5. **The `Z:` drive mapping is configurable** and not universally present.

So path-probing trades determinism for fragility — exactly on the platform (Linux)
it was supposed to help — to save < 0.5 MB.

## Decision: embed the font

Embed **DejaVu Sans** compressed into the DLL via ImGui's `binary_to_compressed_c`
(`generated_shared/dejavu_sans_ttf.h`, ~483 KB) and merge it with
`AddFontFromMemoryCompressedTTF` over the same extended ranges. This renders
**identically on every platform** — Windows, Wine/Proton, Mac — with **zero external
dependency**. Cost: DLL grows ~0.48 MB (5.0 → 5.4 MB; trivial next to the repo's
other data). A system-font merge is kept **only as a last-ditch fallback** if the
embedded decompress ever fails.

### Font choice

- **DejaVu Sans** — license: Bitstream Vera / Arev derivative, freely
  redistributable and **embeddable**, with no OFL "reserved font name" constraint
  (so subsetting is unencumbered).
- One file covers Latin (incl. Extended-A/B with œ), Greek, Cyrillic, punctuation,
  and currency — exactly the ranges the overlay needs.
- ImGui only rasterizes the **glyph ranges we pass** (`kExtRanges`), so the runtime
  atlas stays small regardless of the font file's full coverage.

## Scope and future options

- **European coverage only** (Latin/Greek/Cyrillic/punctuation/currency). CJK / Thai
  / Arabic are deliberately out of scope — they need a dedicated font and a much
  larger atlas. The user base is French; if a CJK-language user ever needs it, the
  clean path is a **runtime fallback to the system/game font for those scripts**, not
  a ~16 MB embed.
- **If DLL size becomes a concern:** subset the embedded TTF to just the ranges above
  with `pyftsubset` (~483 KB → ~100–150 KB), keeping full determinism. Not done yet —
  the current size is acceptable.

## Validation

Log line on startup confirms which path was taken:

```
[FONT] merged extended Unicode glyphs from embedded DejaVu Sans
```

(vs. `embedded font failed; merged from C:\...` for the fallback, or a warning if
both fail). Note that Latin-1 chars like `é`/`à`/`ç` are **not** a valid test — they
render under the default ProggyClean already. The meaningful visual check is a glyph
**outside** `0xFF`: **œ**, an em-dash, or **€**.

## See also

- `src/goblin_overlay.cpp` — the font setup block.
- Memory: `imgui-unicode-font` (the original glyph-coverage finding and this decision).
