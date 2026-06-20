#pragma once
// Category → atlas icon-cell key. The key is a goblin::overlay_icons ICON_CELLS id
// (e.g. "show_bosses"). Returns nullptr when no atlas icon exists for the category
// (the renderer then falls back to a coloured circle). Indexed by the Category enum
// ordinal (generated/goblin_map_data.hpp).

namespace goblin::worldmap
{
// category = static_cast<int>(Category). Out-of-range → nullptr.
const char *category_icon_key(int category);

// Circle-fallback colour (packed ImU32 ABGR) for the category, by broad group. Only
// shows where the category has no atlas icon; out-of-range → a neutral default.
unsigned int category_color(int category);
} // namespace goblin::worldmap
