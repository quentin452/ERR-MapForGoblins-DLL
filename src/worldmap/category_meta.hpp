#pragma once
// Category → atlas icon-cell key. The key is a goblin::overlay_icons ICON_CELLS id
// (e.g. "show_bosses"). Returns nullptr when no atlas icon exists for the category
// (the renderer then falls back to a coloured circle). Indexed by the Category enum
// ordinal (generated/goblin_map_data.hpp).

namespace goblin::worldmap
{
// category = static_cast<int>(Category). Out-of-range → nullptr.
const char *category_icon_key(int category);
} // namespace goblin::worldmap
