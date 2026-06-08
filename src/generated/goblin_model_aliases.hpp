#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{

// Gather-asset model aliases (same physical node, different model ids). ERR substitutes
// some base assets with DLC-era models at runtime; GEOF save entries then carry the
// substituted model's hash. When GEOF's model prefix has no marker rows on a tile, try
// its aliases. Built from shared goodsId across aeg099/aeg463 item mappings.
struct ModelAlias
{
    uint32_t model_id;  // model id as found in a GEOF entry (e.g. 10463860 = AEG463_860)
    uint32_t alias_id;  // equivalent model id used by marker rows (e.g. 10099753)
};

extern const ModelAlias MODEL_ALIASES[];  // sorted by model_id
extern const size_t MODEL_ALIAS_COUNT;

} // namespace goblin::generated
