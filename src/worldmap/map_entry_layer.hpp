#pragma once
// MapEntryLayer — a MarkerLayer over the baked MAP_ENTRIES, one instance per
// category. All categories share a single bucketed cache (built once). visible()
// polls the live per-category toggle so the F1 menu drives overlay markers.

#include "marker_layer.hpp"

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

// Recompute the per-category census (total collectible + looted) from the overlay's
// OWN marker buckets — the same markers + collected detection the renderer grays — and
// publish it via goblin::ui::set_category_census. Logs [OVERLAY-CENSUS]. Called by
// refresh_category_census in overlay-only mode so the F1 badge matches the map.
void refresh_overlay_census();
} // namespace goblin::worldmap
