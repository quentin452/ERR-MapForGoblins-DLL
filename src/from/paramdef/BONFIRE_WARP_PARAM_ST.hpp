/* Hand-authored PARTIAL paramdef (head fields only) for live grace reading.
 * BonfireWarpParam rows are BYTE-PACKED (posX at the unaligned 0x24) → #pragma pack(1).
 * Offsets VERIFIED against the live IN-MEMORY row bytes (the [BONFIRE-PROBE] dump), which
 * is what get_param actually reads — NOT the XML or a SoulsFormats file re-serialize (those
 * mis-anchored the analysis). get_param reinterprets the row pointer and strides by the
 * param's own row size, so a partial head struct is fine.
 *
 * Verified row id=100000: 04:eventflagId=71000  1c:iconId=1  1e:dispMask00=1  20:areaNo=10
 *                         24:posX=-232.76  2c:posZ=345.13  30:textId1=100000. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace from {
namespace paramdef {

#pragma pack(push, 1)
struct BONFIRE_WARP_PARAM_ST {
    uint8_t  _pad00[4];           // 0x00 disableParam_NT:1 + reserve1:7 + reserve2[3]
    uint32_t eventflagId;         // 0x04 unlock/discovery flag (set when the grace is reached)
    uint32_t bonfireEntityId;     // 0x08
    uint8_t  _pad0c[4];           // 0x0c pad4[2] + bonfireSubCategorySortId(u16)
    uint16_t forbiddenIconId;     // 0x10 (2=normal-undiscovered, 64=underground-undiscovered)
    uint8_t  dispMinZoomStep;     // 0x12
    uint8_t  selectMinZoomStep;   // 0x13
    int32_t  bonfireSubCategoryId;// 0x14
    uint32_t clearedEventFlagId;  // 0x18
    uint16_t iconId;              // 0x1c 1=normal grace, 44=underground/cave grace, 48=unique
    uint8_t  dispMask0;           // 0x1e bit0=dispMask00, bit1=dispMask01
    uint8_t  dispMask1;           // 0x1f bit0=dispMask02
    uint8_t  areaNo;              // 0x20
    uint8_t  gridXNo;             // 0x21
    uint8_t  gridZNo;             // 0x22
    uint8_t  _pad23;              // 0x23
    float    posX;                // 0x24
    float    posY;                // 0x28
    float    posZ;                // 0x2c
    int32_t  textId1;             // 0x30 grace place-name msg id
};
#pragma pack(pop)

static_assert(offsetof(BONFIRE_WARP_PARAM_ST, eventflagId) == 0x04, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, bonfireEntityId) == 0x08, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, forbiddenIconId) == 0x10, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, bonfireSubCategoryId) == 0x14, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, clearedEventFlagId) == 0x18, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, iconId) == 0x1c, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, dispMask0) == 0x1e, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, areaNo) == 0x20, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, gridXNo) == 0x21, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, gridZNo) == 0x22, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, posX) == 0x24, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, posY) == 0x28, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, posZ) == 0x2c, "BonfireWarp layout");
static_assert(offsetof(BONFIRE_WARP_PARAM_ST, textId1) == 0x30, "BonfireWarp layout");
static_assert(sizeof(BONFIRE_WARP_PARAM_ST) == 0x34, "BonfireWarp head size");

} // namespace paramdef
} // namespace from
