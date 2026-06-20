#pragma once
// Pure, dependency-free map-space -> backbuffer-pixel projection.
//
// Extracted from goblin_overlay.cpp::project_uv so it can be unit-tested off-target
// (no Windows / ImGui / live game). The SOLVED engine projection is documented in
// docs/re/windows_worldmap_viewcenter_re_findings.md §0:
//
//   screen_local = marker * zoom - pan     // pan @ WorldMapArea +0x378/+0x37C
//   backbuffer   = screen_local * scale + bias
//
// pan is ALREADY in screen-local px -> subtract DIRECTLY (NOT (marker-pan)*zoom; the
// extra *zoom on pan was the documented FAILED attempt). scale/bias are the per-axis
// affine to the real backbuffer (recovered by calibration / tested below).

namespace goblin::projection
{
struct Px
{
    float x, y;
};

// marker (mU,mV) in render/map space -> backbuffer pixels.
inline Px project(float mU, float mV, float zoom, float panX, float panZ,
                  float scaleX, float scaleY, float biasX, float biasY)
{
    const float sx = mU * zoom - panX; // screen-local px
    const float sz = mV * zoom - panZ;
    return Px{sx * scaleX + biasX, sz * scaleY + biasY};
}

// Engine inverse (FUN_1409cd0a0): screen-local px -> marker space. marker = (screen+pan)/zoom.
inline void unproject_local(float localX, float localZ, float zoom, float panX, float panZ,
                            float &mU, float &mV)
{
    mU = (localX + panX) / zoom;
    mV = (localZ + panZ) / zoom;
}

// Mirror of goblin_overlay.cpp::project_uv: the exact pan_center branch switch, so a test
// guards the real code path. pan_center=true -> pan-anchored (correct); false -> reticle-
// coupled (the bug). reticleU/V = cursor centre fields (+0xFC/+0x100), halfW/H = canvas/2.
inline Px project_uv(bool pan_center, float mU, float mV, float zoom,
                     float panX, float panZ, float reticleU, float reticleV,
                     float halfW, float halfH, float scaleX, float scaleY,
                     float biasX, float biasY)
{
    if (pan_center)
        return Px{(mU * zoom - panX) * scaleX + biasX, (mV * zoom - panZ) * scaleY + biasY};
    return Px{(mU - reticleU) * zoom * scaleX + halfW + biasX,
              (mV - reticleV) * zoom * scaleY + halfH + biasY};
}

// RETICLE-COUPLED projection (the BUGGY legacy branch, goblin_overlay.cpp project_uv else
// path): anchors markers to the reticle/cursor centre (cursor +0xFC/+0x104) instead of pan.
//   screen = (marker - reticle)*zoom + canvasHalf + bias
// Because the reticle tracks the MOUSE (RE §1), this moves every marker whenever the mouse
// moves -- even when pan/zoom (the real view state) are frozen. That is the camera-still,
// mouse-to-edge drift. Kept here only so the data-driven test can reproduce it.
inline Px project_reticle(float mU, float mV, float zoom, float reticleU, float reticleV,
                          float halfW, float halfH, float scaleX, float scaleY,
                          float biasX, float biasY)
{
    return Px{(mU - reticleU) * zoom * scaleX + halfW + biasX,
              (mV - reticleV) * zoom * scaleY + halfH + biasY};
}
} // namespace goblin::projection
