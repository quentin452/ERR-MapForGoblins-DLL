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

// ─────────────────────────────────────────────────────────────────────────────
// BAKED projection API (the SOLVED model, be1bd9c). This is what the overlay uses.
// The legacy free functions further down are the RE-era helpers kept only so the
// off-target drift/lag tests still build.
// ─────────────────────────────────────────────────────────────────────────────

// Per-axis residual affine to the real backbuffer. Both ≈ identity once the canvas
// factor is applied (scale 1.0 / bias 0 proven end-to-end); kept as a struct so a
// future per-resolution tweak has a home without touching call sites.
struct Calib
{
    float scaleX = 1.f, scaleY = 1.f, biasX = 0.f, biasY = 0.f;
};

// The live world-map view, reduced to just what projection needs (so this header
// stays free of the probe / Windows / ImGui). pan @ WorldMapArea +0x378/+0x37C,
// zoom @ +0x380; snapMid = midpoint of the snap-rect +0x340..+0x34c.
struct View
{
    float panX = 0, panZ = 0, zoom = 1;
    float snapMidX = 0, snapMidZ = 0;
};

// View centre in marker space = (pan + snapMid)/zoom. This is the engine pan setter
// FUN_1409cd100 inverted (pan = zoom·viewCentre − snapMid), so it is CURSOR-
// INDEPENDENT — it works for mouse AND gamepad (the reticle +0xFC only tracks the
// mouse). Using it as the projection centre is the fix that killed the camera-frozen
// / mouse-to-edge marker drift.
inline void view_center(const View &v, float &cU, float &cV)
{
    cU = (v.panX + v.snapMidX) / v.zoom;
    cV = (v.panZ + v.snapMidZ) / v.zoom;
}

// marker (mU,mV) in render/map space → backbuffer pixels. BAKED model:
//   screen = (marker − viewCentre)·zoom·(real/virtual)·scale + real/2 + bias·(real/virtual)
// The engine renders the map in a FIXED virtual 1920×1080 GFx canvas then scales to the
// backbuffer, so the (realW/1920, realH/1080) CANVAS FACTOR is mandatory (measured via CT,
// constant across zoom). bias is stored in 1920×1080-reference px and scaled the same way,
// so a calibration tuned at one resolution holds at every resolution.
inline Px project_screen(float mU, float mV, const View &v, float realW, float realH,
                         const Calib &c = {})
{
    float cU, cV;
    view_center(v, cU, cV);
    const float kx = realW / 1920.f, ky = realH / 1080.f;
    return Px{(mU - cU) * v.zoom * kx * c.scaleX + realW * 0.5f + c.biasX * kx,
              (mV - cV) * v.zoom * ky * c.scaleY + realH * 0.5f + c.biasY * ky};
}

// Inverse of project_screen: backbuffer pixels → marker/map space. Used to unproject the screen
// viewport corners into a map-space rect for viewport culling (the marker hot loop skips markers
// whose map-space cell is off that rect). Exact inverse of the affine above; degenerate scales
// (zoom/scale ~0) are guarded so a closed/uninitialised view can't divide by zero.
inline void unproject_screen(float px, float py, const View &v, float realW, float realH,
                             float &mU, float &mV, const Calib &c = {})
{
    float cU, cV;
    view_center(v, cU, cV);
    const float kx = realW / 1920.f, ky = realH / 1080.f;
    const float dx = v.zoom * kx * c.scaleX, dy = v.zoom * ky * c.scaleY;
    mU = (dx != 0.f) ? cU + (px - realW * 0.5f - c.biasX * kx) / dx : cU;
    mV = (dy != 0.f) ? cV + (py - realH * 0.5f - c.biasY * ky) / dy : cV;
}

// MOTION-SYNC frame delay. The whole game frame (map AND its cursor) is composited
// ~1 frame behind our Present sample, so map-bound markers need a −1.0-frame view
// DELAY to sit on the map. Implemented as a frame-history ring buffer: sample
// |delay| frames back (fractional → lerp). A true past value never overshoots on a
// sharp pan reversal (keyboard/gamepad ZQSD = instant velocity flips), unlike
// extrapolation. delay is in frames, ≥ 0; the baked value is 1.0.
template <int N = 8>
struct ViewDelay
{
    float cU[N]{}, cV[N]{}, z[N]{};
    int head = 0;
    bool init = false;

    void reset() { init = false; }

    // Push the live view, rewrite v to the delayed view (centre+zoom back into pan).
    // delayZoom: when false, the CENTER (pan) is delayed but ZOOM uses the current frame's value.
    // Zoom on the map is a discrete wheel step that the engine applies to the basemap immediately,
    // so delaying zoom holds markers at the previous scale for one frame = a radial teleport per
    // notch. Delaying only the center keeps pan-sync without the zoom-step teleport. (If the engine
    // instead EASES zoom over several frames, the read value is the target and no frame-delay matches
    // — that needs reading the eased zoom, a separate fix.)
    void apply(View &v, float delay, bool delayZoom = true)
    {
        float liveU, liveV;
        view_center(v, liveU, liveV);
        if (!init)
        {
            for (int i = 0; i < N; ++i) { cU[i] = liveU; cV[i] = liveV; z[i] = v.zoom; }
            init = true;
        }
        head = (head + 1) % N;
        cU[head] = liveU; cV[head] = liveV; z[head] = v.zoom;

        float d = delay;
        if (d < 0) d = 0;
        if (d > N - 1) d = N - 1;
        int d0 = (int)d; float fr = d - d0;
        int i0 = (head - d0 + 2 * N) % N;
        int i1 = (head - d0 - 1 + 2 * N) % N;
        float eU = cU[i0] * (1 - fr) + cU[i1] * fr;
        float eV = cV[i0] * (1 - fr) + cV[i1] * fr;
        float eZ = delayZoom ? (z[i0] * (1 - fr) + z[i1] * fr) : v.zoom;
        v.zoom = eZ;
        v.panX = eU * eZ - v.snapMidX;
        v.panZ = eV * eZ - v.snapMidZ;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// LEGACY RE-era helpers — kept only so the off-target drift/lag unit tests
// (tests/test_projection.cpp, tests/test_lag_capture.cpp) still build. NOT used by
// the overlay anymore; do not add new callers. The baked API above supersedes them.
// ─────────────────────────────────────────────────────────────────────────────

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
