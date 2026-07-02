#pragma once
// Overlay UI localization (v1). tr() translates OUR UI strings — panel labels, tooltips,
// marker-tooltip glue ("Unknown item", "3/12 left") — keyed by their exact English source
// string. Game CONTENT (item/place/boss names) comes from the game's own FMGs and is never
// translated here.
//
// The table loads once at init from `<mod folder>/lang/<code>.txt` (code from ini
// overlay_language; "auto" = OS UI language, "en"/missing file = table off, tr() = identity).
// File format, UTF-8, line-oriented:
//     # comment
//     en=<English source string, newlines escaped as \n>
//     tr=<translation, same escaping>
// A missing/empty tr falls back to English per-string. Translations of printf-style
// format strings MUST keep the same % placeholders in the same order.
//
// Host-owned (loaded before the render module draws; immutable afterwards), exported to the
// render DLL like the rest of the config surface. tr() returns either the stored translation
// or the input pointer itself — both stay valid for the process lifetime.

#include "goblin_dll_export.hpp"

#include <filesystem>

namespace goblin::i18n
{
// Translate an overlay UI string (English = the key). Identity when no table is active
// or the string has no entry.
GOBLIN_RENDER_API const char *tr(const char *en);

// True when a non-English table is loaded (lets callers skip double-matching work).
GOBLIN_RENDER_API bool active();

// Resolve the language + load the table. Host init only (dllmain, after load_config).
void initialize(const std::filesystem::path &mod_folder);
}
