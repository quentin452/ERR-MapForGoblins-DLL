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

// ── Baked → GPU icon migration tracking (F1 completion panel) ───────────────────
// Total number of categories (Category enum size).
int category_count();

// True when the category's icon_key resolves to a real BAKED atlas cell (vs the
// by-design circle-fallback categories, which have no cell). = "has a baked icon".
bool category_has_baked_icon(int category);

// Canonical engine iconId for the category's icon, for the baked→GPU migration.
// 0 = NOT mapped yet (still baked-only → "to replace"). Sparse: fill an entry as each
// category's real engine sprite is wired. Combined with harvested_icon() this drives
// the F1 "X / N icons replaced" completion counter.
int category_gpu_iconId(int category);

// Name-keyed engine map symbol for the category (ERR custom MENU_MAP_ERR_* / vanilla MENU_MAP_*),
// or nullptr. Sparse — only categories with a real game symbol. Resolved via map_icon_rect_by_name.
const char *category_gpu_icon_name(int category);

// True when the category draws via a NATIVE engine sprite instead of the baked atlas — i.e. it is
// already migrated off the bake. SINGLE SOURCE OF TRUTH mirroring what the renderer actually draws
// (map_renderer IconSet::resolve + GraceLayer): a name-keyed map symbol (category_gpu_icon_name),
// a numeric map-point iconId (category_gpu_iconId), OR the dedicated grace-sprite path (WorldGraces,
// drawn by GraceLayer from s_grace_tex / MENU_MAP_ERR_GraceUnderground). Drives the F1 migration
// panel — keep in lock-step with the render policy so the count never desyncs from reality.
bool category_is_gpu_native(int category);
} // namespace goblin::worldmap
