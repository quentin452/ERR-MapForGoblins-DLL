#pragma once
// MapEntryLayer — a MarkerLayer over the baked MAP_ENTRIES, one instance per
// category. All categories share a single bucketed cache (built once). visible()
// polls the live per-category toggle so the F1 menu drives overlay markers.

#include "marker_layer.hpp"

#include <string>

namespace goblin::worldmap
{
class MapEntryLayer : public MarkerLayer
{
public:
    explicit MapEntryLayer(int category); // category = static_cast<int>(Category)
    const char *category() const override { return name_; }
    bool visible() const override;
    const std::vector<Marker> &markers() const override;

private:
    int cat_;
    const char *name_;
};

// Build every category's marker cache once, up front (thread-safe via call_once).
// Called from setup_mod after the params are ready so the disk-MSB loot parse +
// projection happen on the init thread, NOT on the first world-map open — kills
// the first-open hitch. A no-op if the lazy markers()/census path already built.
void prebuild_markers();

// Force a fresh bucket rebuild (disk source only) so a config change affecting bucket content —
// e.g. stackIdenticalItems — applies without a map reload. Called from the F1 menu on toggle.
void rebuild_markers();

// D-far -1 diagnostic (far_terrain_relief_plan.md): dump the MSB placement Y-cloud distribution per
// overworld tile from the already-parsed cache, to decide BEFORE building the far-relief grid whether
// posY is usable directly (world-ish, smooth across tiles) or block-local (per-tile origin → stepped).
// No new parse. Returns a summary; detail goes to the [FARRELIEF] log. RPC `far_relief_probe`.
std::string far_relief_probe();

// Recompute the per-category census (total collectible + looted) from the overlay's
// OWN marker buckets — the same markers + collected detection the renderer grays — and
// publish it via goblin::ui::set_category_census. Logs [OVERLAY-CENSUS]. Called by
// refresh_category_census in overlay-only mode so the F1 badge matches the map.
void refresh_overlay_census();

// Resolve an MSB EntityID to a projected world position, from the cache built during
// the disk-marker worker pass (enemy entities first, then asset/collectible entities —
// same precedence the disk loot pipeline uses). Returns false if the entity is unknown
// (not placed on the loaded map, or the worker hasn't built buckets yet). worldX/worldZ
// are already projected (marker_world_pos), group is the overlay marker group
// (marker_group_from) — both ready to assign onto a Marker directly, no extra
// transform needed by the caller. worldY (optional out) is the placement's BLOCK-LOCAL
// MSB Y (Part+0x20[1], same frame as Marker.worldY) for the altitude badge — quest-NPC
// pins had no badge because this path dropped Y (user report 2026-07-02).
bool entity_world_pos(uint32_t entity_id, float &worldX, float &worldZ, int &group,
                      float *worldY = nullptr);

// One item sold by SOME merchant, harvested from the live ShopLineupParam for the F1
// item-search "· buyable" rows (merchant_item_search_plan.md, Slice 1). name_id is the
// marker offset-encoded id (encode_live_item) so the search's lookup_text_utf8 / English
// alias resolve it for free. `infinite` = at least one shop sells it unlimited (sellQuantity
// == -1). `gated` = EVERY shop row for it is behind an eventFlag_forStock unlock (bell
// bearing / progression) — i.e. not freely buyable yet.
struct MerchantItem { int32_t name_id; bool infinite; bool gated; };

// Deduped list of merchant-sold items, rebuilt each bucket build (build_buckets_impl).
// Empty if ShopLineupParam is absent. Read by the F1 item search on the present thread.
const std::vector<MerchantItem> &merchant_search_items();
} // namespace goblin::worldmap
