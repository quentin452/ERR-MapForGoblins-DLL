#pragma once
// GraceLayer — the first MarkerLayer impl: every grace from the live
// WorldMapPointParam, projected to unified world coords and group-classified once.

#include "marker_layer.hpp"

namespace goblin::worldmap
{
class GraceLayer : public MarkerLayer
{
public:
    const char *category() const override { return "Graces"; }
    bool visible() const override { return true; } // TODO: wire to a category toggle
    const std::vector<Marker> &markers() const override;

private:
    mutable std::vector<Marker> cache_;
    mutable bool built_ = false;
};
} // namespace goblin::worldmap
