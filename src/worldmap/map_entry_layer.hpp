#pragma once
// MapEntryLayer — a MarkerLayer over the baked MAP_ENTRIES, one instance per
// category. All categories share a single bucketed cache (built once). visible()
// polls the live per-category toggle so the F1 menu drives overlay markers.

#include "marker_layer.hpp"
#include "../goblin_heightfield.hpp"   // heightfield::Cell (D-far -1 MSB Y-cloud relief snapshot)

#include <string>
#include <unordered_set>
#include <vector>

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

// Enemy-name probe: dump the live MSB-scanned enemies (g_parsed.enemies) that match a query, with each
// one's npcParam, model, tile, and RESOLVED display name + tier. Query is either a cell "area gx gz"
// (three ints) or a case-insensitive substring of the part-name OR resolved name. Detail → [ENAME] log;
// returns a match-count summary. RPC `vmap ename <query>`. Diagnoses why a clone (e.g. Godefroy) resolves
// to its base's name (Godrick) and spawns a duplicate boss marker in build_live_bosses' supplement pass.
std::string ename_probe(const std::string &query);

// Item PROVENANCE probe: for a lot id (numeric) or item-name substring, dump every disk placement
// (Treasure / cross-tile LOD-Treasure / AEG Collectible) carrying that lot — RAW source tile + pos and
// PROJECTED overworld position (onmap / OOB / DECLINED). Reads the cached g_parsed (no re-scan). RPC
// `vmap prov <lot|name>`. Diagnoses duplicate / mis-projected loot (e.g. a lot present in several ERR
// Roundtable-copy tiles → the same item at multiple world positions). Detail → [PROV] log.
std::string loot_prov_probe(const std::string &query);

// D-far -1 v0 — build the MSB Y-cloud ground-height field (overworld collectible posY → per-cell median
// grid → heightfield::Cell[] with gradient normals). Consumed by the vmap hillshade (same as the near
// raycast). cellSize in world units (default 128). RPC `far_relief [cell]`; snapshot for the renderer.
std::string build_far_relief(int group, int cellSize);
size_t far_relief_snapshot(std::vector<goblin::heightfield::Cell> &out);
float far_relief_step();
int far_relief_built_group();   // vmap group the current field was built for (-1 = none)

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
// `seller_name_id` = the NpcName key (+700M offset-encoded) of ONE merchant that sells it,
// resolved via the merchant-pin shop-range join (Slice 3) — 0 when the seller is unknown
// (worldFeaturesFromDisk off, or the shop row is outside every OpenRegularShop range).
struct MerchantItem { int32_t name_id; bool infinite; bool gated; int32_t seller_name_id; };

// Deduped list of merchant-sold items, rebuilt each bucket build (build_buckets_impl).
// Empty if ShopLineupParam is absent. Read by the F1 item search on the present thread.
const std::vector<MerchantItem> &merchant_search_items();

// name_ids (+700M NpcName keys) of the PINNED WorldMerchant markers, rebuilt each bucket
// build. QuestNpcLayer's runtime-fallback pins dedup against this — a merchant NPC keeps
// ONE glyph (the merchant pin) instead of merchant + name-only fallback twins. Hand-authored
// quest STEP pins are not deduped (they add step prose and share the same glyph).
const std::unordered_set<int32_t> &merchant_pinned_names();
} // namespace goblin::worldmap
