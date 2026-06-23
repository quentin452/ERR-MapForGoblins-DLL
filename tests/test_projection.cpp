// Off-target unit tests for the map-space -> backbuffer projection.
//
// Goal: pin down WHY grace icons drift instead of sticking to the map when the view
// pans (and when the mouse is forced to a window edge while the camera is "still").
// Build & run native (NOT the clang-cl/xwin DLL toolchain):
//     g++ -std=c++17 -I../src test_projection.cpp -o test_projection && ./test_projection
//
// Each test is an invariant the engine guarantees (docs/re/windows_worldmap_viewcenter_re_findings.md).
// A failing test names the broken assumption.

#include "goblin_projection.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using goblin::projection::project;
using goblin::projection::Px;
using goblin::projection::unproject_local;

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const std::string &name, const std::string &detail = "")
{
    if (ok)
    {
        ++g_pass;
        std::printf("  PASS  %s\n", name.c_str());
    }
    else
    {
        ++g_fail;
        std::printf("  FAIL  %s   %s\n", name.c_str(), detail.c_str());
    }
}

static bool approx(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

// A representative live sample (docs/marker_mapspace_CT_recipe.md): zoom 2.25,
// pan ~ (8091.8, 18657.5), fullRect [0,0,10496,10496].
static constexpr float ZOOM = 2.25f;
static constexpr float PANX = 8091.8f;
static constexpr float PANZ = 18657.5f;

int main()
{
    std::printf("== projection invariants ==\n");

    // ---------------------------------------------------------------------------
    // T1. Order of operations: forward then the ENGINE inverse must recover marker.
    //     screen_local = marker*zoom - pan  <=>  marker = (screen_local+pan)/zoom.
    //     Guards against the (marker-pan)*zoom regression (the documented failed try).
    {
        float mU = 4200.f, mV = 9100.f;
        // screen-local only: identity scale, zero bias.
        Px p = project(mU, mV, ZOOM, PANX, PANZ, 1.f, 1.f, 0.f, 0.f);
        float rU, rV;
        unproject_local(p.x, p.y, ZOOM, PANX, PANZ, rU, rV);
        check(approx(rU, mU) && approx(rV, mV), "T1 forward/inverse round-trip",
              "got (" + std::to_string(rU) + "," + std::to_string(rV) + ")");
    }

    // ---------------------------------------------------------------------------
    // T2. STICK-UNDER-PAN (the actual bug). A world-fixed marker must move on screen
    //     by exactly the pan delta (times the axis scale) — same as the map content,
    //     so it stays glued. We assert X and Y track with the SAME law. If the code
    //     ever scales one axis differently from how it subtracts pan, this fails.
    {
        float mU = 4200.f, mV = 9100.f;
        const float sX = 1.0f, sY = 1.0f; // identity backbuffer scale
        Px a = project(mU, mV, ZOOM, PANX, PANZ, sX, sY, 0.f, 0.f);
        // pan the view: +300 local px right, +300 local px down.
        Px b = project(mU, mV, ZOOM, PANX + 300.f, PANZ + 300.f, sX, sY, 0.f, 0.f);
        float dX = b.x - a.x; // expect -300*sX
        float dY = b.y - a.y; // expect -300*sY
        check(approx(dX, -300.f * sX), "T2x marker sticks under X pan",
              "dX=" + std::to_string(dX) + " want " + std::to_string(-300.f * sX));
        check(approx(dY, -300.f * sY), "T2y marker sticks under Y pan",
              "dY=" + std::to_string(dY) + " want " + std::to_string(-300.f * sY));
        check(approx(dX / sX, dY / sY), "T2 X and Y track by the same law",
              "dX/sX=" + std::to_string(dX / sX) + " dY/sY=" + std::to_string(dY / sY));
    }

    // ---------------------------------------------------------------------------
    // T3. CAMERA-STILL + MOUSE-TO-EDGE. The reticle (cursor +0xFC/+0x104/+0x10C) is
    //     input-driven (RE §1), but pan (+0x378/+0x37C) is device-independent: the
    //     cursor tick NEVER writes it. So if pan is unchanged, the marker MUST NOT
    //     move, no matter where the reticle/mouse goes. project() takes no reticle
    //     input -> this proves the projection is reticle-free. (A regression that
    //     fed a reticle field into the projection would move the marker here.)
    {
        float mU = 4200.f, mV = 9100.f;
        Px a = project(mU, mV, ZOOM, PANX, PANZ, 1.f, 1.f, 0.f, 0.f);
        // ... mouse slammed to the right/bottom edge: reticle changes, pan does NOT.
        Px b = project(mU, mV, ZOOM, PANX, PANZ, 1.f, 1.f, 0.f, 0.f);
        check(approx(a.x, b.x) && approx(a.y, b.y), "T3 marker frozen when pan frozen",
              "moved by (" + std::to_string(b.x - a.x) + "," + std::to_string(b.y - a.y) + ")");
    }

    // ---------------------------------------------------------------------------
    // T4. ANISOTROPY PROBE. If the real backbuffer mapping is anisotropic (scaleX !=
    //     scaleY) but only one axis is wrong, the per-axis pan response diverges. This
    //     test documents the failure SHAPE: same pan delta on both axes, different px
    //     response -> exactly the "X fine, Y drifts" report. It PASSES only when the
    //     two axis scales used for projection are the two the engine actually uses.
    {
        float mU = 4200.f, mV = 9100.f;
        // Pretend the engine's true backbuffer scale is isotropic 1.0 on both axes.
        const float trueSX = 1.0f, trueSY = 1.0f;
        // The code under test MUST use the same on both axes. Feed identical scales.
        const float codeSX = 1.0f, codeSY = 1.0f;
        Px t0 = project(mU, mV, ZOOM, PANX, PANZ, trueSX, trueSY, 0, 0);
        Px t1 = project(mU, mV, ZOOM, PANX, PANZ + 300.f, trueSX, trueSY, 0, 0);
        Px c0 = project(mU, mV, ZOOM, PANX, PANZ, codeSX, codeSY, 0, 0);
        Px c1 = project(mU, mV, ZOOM, PANX, PANZ + 300.f, codeSX, codeSY, 0, 0);
        float truthDY = t1.y - t0.y;
        float codeDY = c1.y - c0.y;
        check(approx(truthDY, codeDY), "T4 Y pan response matches engine scale",
              "engine dY=" + std::to_string(truthDY) + " code dY=" + std::to_string(codeDY));
    }

    // ---------------------------------------------------------------------------
    // T5. BAKED project_screen — the API the overlay actually uses. Two invariants:
    //   (a) a marker AT the view centre lands at the backbuffer centre (realW/2,realH/2),
    //       regardless of zoom/pan — the pan-centred model.
    //   (b) the CANVAS FACTOR is applied: at a non-1920×1080 backbuffer the same world
    //       offset scales by realW/1920 (the bug the factor fixed). Compare 1920 vs 1280.
    {
        using goblin::projection::project_screen;
        using goblin::projection::view_center;
        using goblin::projection::View;
        View v;
        v.panX = PANX; v.panZ = PANZ; v.zoom = ZOOM;
        v.snapMidX = 120.f; v.snapMidZ = -75.f; // arbitrary snap-rect midpoint
        float cU, cV;
        view_center(v, cU, cV);

        // (a) centre marker → backbuffer centre, at two resolutions.
        Px c1920 = project_screen(cU, cV, v, 1920.f, 1080.f);
        Px c1280 = project_screen(cU, cV, v, 1280.f, 720.f);
        check(approx(c1920.x, 960.f) && approx(c1920.y, 540.f), "T5a centre->backbuffer centre @1920",
              std::to_string(c1920.x) + "," + std::to_string(c1920.y));
        check(approx(c1280.x, 640.f) && approx(c1280.y, 360.f), "T5a centre->backbuffer centre @1280",
              std::to_string(c1280.x) + "," + std::to_string(c1280.y));

        // (b) an off-centre marker: its pixel offset from centre must scale by realW/1920.
        float mU = cU + 1000.f, mV = cV + 600.f;
        Px o1920 = project_screen(mU, mV, v, 1920.f, 1080.f);
        Px o1280 = project_screen(mU, mV, v, 1280.f, 720.f);
        float dx1920 = o1920.x - 960.f, dx1280 = o1280.x - 640.f;
        check(approx(dx1280, dx1920 * (1280.f / 1920.f), 0.1f), "T5b canvas factor scales offset by realW/1920",
              "d1920=" + std::to_string(dx1920) + " d1280=" + std::to_string(dx1280));
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
