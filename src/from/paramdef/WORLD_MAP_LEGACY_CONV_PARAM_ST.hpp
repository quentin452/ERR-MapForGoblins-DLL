/* This file was hand-written from tools/paramdefs/WorldMapLegacyConvParam.xml. */
#pragma once

namespace from {
namespace paramdef {
/**
 * @brief Legacy-dungeon -> overworld conversion (one row per src base point).
 *
 * Maps a non-overworld sub-area block (srcArea,srcGrid) onto another area's
 * block (dstArea,dstGrid). Chains are possible (m35 -> 11 -> 60); the terminal
 * is any overworld/field area in [50,88]. Layout matches the engine's row
 * (0x30 bytes); see docs/re/windows_legacyconv_param_live_re_findings.md §1.
 */
struct WORLD_MAP_LEGACY_CONV_PARAM_ST {
    bool disableParam_NT : 1 { false };
    unsigned char disableParamReserve1 : 7;
    unsigned char disableParamReserve2[3];

    /** @brief AA part of mAA_BB_CC_DD — SRC block area. */
    unsigned char srcAreaNo{ 0 };
    /** @brief BB — SRC grid X. */
    unsigned char srcGridXNo{ 0 };
    /** @brief CC — SRC grid Z. */
    unsigned char srcGridZNo{ 0 };
    unsigned char pad1[1];

    /** @brief SRC base-point world X. */
    float srcPosX{ 0.f };
    /** @brief SRC base-point world Y (unused). */
    float srcPosY{ 0.f };
    /** @brief SRC base-point world Z. */
    float srcPosZ{ 0.f };

    /** @brief AA — DST block area. */
    unsigned char dstAreaNo{ 0 };
    /** @brief BB — DST grid X. */
    unsigned char dstGridXNo{ 0 };
    /** @brief CC — DST grid Z. */
    unsigned char dstGridZNo{ 0 };
    unsigned char pad2[1];

    /** @brief DST base-point world X. */
    float dstPosX{ 0.f };
    /** @brief DST base-point world Y (unused). */
    float dstPosY{ 0.f };
    /** @brief DST base-point world Z. */
    float dstPosZ{ 0.f };

    /** @brief Each srcMapId has exactly one base point. */
    bool isBasePoint : 1 { false };
    unsigned char pad3 : 7;
    unsigned char pad4[11];
};

}; // namespace paramdef
}; // namespace from

static_assert(sizeof(from::paramdef::WORLD_MAP_LEGACY_CONV_PARAM_ST) == 0x30,
    "WORLD_MAP_LEGACY_CONV_PARAM_ST paramdef size does not match detected size");
