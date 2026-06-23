/* Minimal paramdef for WorldMapPieceParam — only the fields the fog-reveal gate reads.
 * Offsets are engine-confirmed (docs/re/windows_fog_reveal_mask_re_findings.md): a
 * standard paramdef opens with disableParam_NT(1b)+reserves(3b) = 4 bytes, so the first
 * real field lands at +0x4. The trailing acquisition* fields are omitted — get_param just
 * reinterprets the (larger) row, so a partial struct is fine for reading the head. */
#pragma once

namespace from {
namespace paramdef {
struct WORLD_MAP_PIECE_PARAM_ST {
    unsigned char disableParam_NT : 1;       // +0x0
    unsigned char disableParamReserve1 : 7;
    unsigned char disableParamReserve2[3];   // +0x1..0x3
    unsigned int openEventFlagId{ 0 };       // +0x4  reveal flag (0 = ungated/always shown)
    float openTravelAreaLeft{ 0.f };         // +0x8  rect Xmin (map-space)
    float openTravelAreaRight{ 0.f };        // +0xc  rect Xmax
    float openTravelAreaTop{ 0.f };          // +0x10 rect Ymin
    float openTravelAreaBottom{ 0.f };       // +0x14 rect Ymax
};
} // namespace paramdef
} // namespace from
