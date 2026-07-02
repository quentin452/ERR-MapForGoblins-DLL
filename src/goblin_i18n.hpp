#pragma once
// Overlay UI localization (v1). tr() translates OUR UI strings — panel labels, tooltips,
// marker-tooltip glue ("Unknown item", "3/12 left") — keyed by their exact English source
// string. Game CONTENT (item/place/boss names) comes from the game's own FMGs and is never
// translated here.
//
// The table loads at init from `<mod folder>/lang/<code>.txt` (code from ini
// overlay_language; "auto" = OS UI language, "en"/missing file = table off, tr() = identity)
// and can be SWAPPED LIVE from the F1 panel via set_language(). File format, UTF-8,
// line-oriented:
//     # comment
//     en=<English source string, newlines escaped as \n>
//     tr=<translation, same escaping>
// A missing/empty tr falls back to English per-string. Translations of printf-style
// format strings MUST keep the same % placeholders in the same order.
//
// Host-owned, exported to the render DLL like the rest of the config surface.
//
// THREADING: tr() and set_language() both run on the PRESENT thread (every caller is the
// panel/renderer draw path; initialize() runs before the first frame), so the live swap
// needs no locking. Do not call tr() from other threads.

#include "goblin_dll_export.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace goblin::i18n
{
// Translate an overlay UI string (English = the key). Identity when no table is active
// or the string has no entry.
GOBLIN_RENDER_API const char *tr(const char *en);

// True when a non-English table is loaded (lets callers skip double-matching work).
GOBLIN_RENDER_API bool active();

// Bumps on every (re)load — callers holding tr()-derived caches (e.g. the category sort
// order) rebuild when this changes. Starts at 0; initialize()/set_language() increment.
GOBLIN_RENDER_API int generation();

// Live language switch (F1 panel combo): load lang/<code>.txt ("en"/"" clears the table,
// "auto" re-resolves from the OS). Returns false when a requested table file is missing
// (state then = English). Present thread only. Does NOT touch config::overlayLanguage —
// the caller owns the config write + persistence.
GOBLIN_RENDER_API bool set_language(const char *code);

// Language codes with a lang/<code>.txt table on disk (sorted). For the panel combo.
GOBLIN_RENDER_API std::vector<std::string> available_languages();

// Resolve the language from config + load the table. Host init only (dllmain, after
// load_config). Also remembers the mod folder for the live switch.
void initialize(const std::filesystem::path &mod_folder);
}
