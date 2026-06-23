/* Hand-authored PARTIAL paramdef (head fields) for live grace-anchor region/tab.
 * Layout from tools/paramdefs/BonfireWarpSubCategoryParam.xml; head mirrors
 * BonfireWarpParam (disableParam_NT:1 + reserves = 4 bytes at 0x00, then textId@0x04),
 * verified there against live row bytes. tabId is the map sub-page (underground area 12
 * splits 12000/12001/12002), keyed by row id == BonfireWarpParam.bonfireSubCategoryId. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace from {
namespace paramdef {

#pragma pack(push, 1)
struct BONFIRE_WARP_SUB_CATEGORY_PARAM_ST {
    uint8_t  _pad00[4];   // 0x00 disableParam_NT:1 + reserve1:7 + reserve2[3]
    int32_t  textId;      // 0x04 sub-region place-name msg id
    uint16_t tabId;       // 0x08 map sub-page id
    uint16_t sortId;      // 0x0a
    // 0x0c dummy8 pad[4] → row 0x10
};
#pragma pack(pop)

static_assert(offsetof(BONFIRE_WARP_SUB_CATEGORY_PARAM_ST, textId) == 0x04, "subcat layout");
static_assert(offsetof(BONFIRE_WARP_SUB_CATEGORY_PARAM_ST, tabId) == 0x08, "subcat layout");
static_assert(offsetof(BONFIRE_WARP_SUB_CATEGORY_PARAM_ST, sortId) == 0x0a, "subcat layout");

} // namespace paramdef
} // namespace from
