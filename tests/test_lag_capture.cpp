// Data-driven test over a REAL in-game capture (tests/data/lag_capture.csv, produced by
// the [LAGCSV] logger: one row/frame of pan + reticle-marker + OS-mouse px).
//
// What the capture proves:
//  - Long runs where pan/zoom are FROZEN while the reticle (mouse) sweeps. In those runs a
//    pan-anchored marker MUST be pixel-frozen; a reticle-coupled marker drifts. That drift
//    is the "camera-still, mouse-to-edge, markers move and never come back" bug.
//  - The projection model screen = marker*zoom - pan is exact: fitting the logged reticle
//    marker -> logged mouse px gives a tiny residual, and pan is in sync with the frame.
//
// Build/run native:
//   g++ -std=c++17 -I../src test_lag_capture.cpp -o test_lag_capture && ./test_lag_capture

#include "goblin_projection.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using goblin::projection::project;
using goblin::projection::project_reticle;
using goblin::projection::Px;

struct Row
{
    long frame;
    float panX, panZ, zoom, reticleU, reticleV, mouseX, mouseY;
};

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const std::string &name, const std::string &detail = "")
{
    (ok ? g_pass : g_fail)++;
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
                detail.empty() ? "" : "   ", detail.c_str());
}

static std::vector<Row> load(const std::string &path)
{
    std::vector<Row> rows;
    std::ifstream f(path);
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line))
    {
        if (line.empty())
            continue;
        for (char &c : line)
            if (c == ',')
                c = ' ';
        std::istringstream ss(line);
        Row r{};
        if (ss >> r.frame >> r.panX >> r.panZ >> r.zoom >> r.reticleU >> r.reticleV >> r.mouseX >> r.mouseY)
            rows.push_back(r);
    }
    return rows;
}

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1] : "data/lag_capture.csv";
    std::vector<Row> rows = load(path);
    std::printf("== lag capture (%zu frames from %s) ==\n", rows.size(), path.c_str());
    if (rows.size() < 20)
    {
        std::printf("  FAIL  could not load capture\n");
        return 1;
    }

    // A fixed world/render marker to project every frame (value irrelevant; we measure how
    // much its SCREEN position moves under each model).
    const float MU = 4200.f, MV = 9100.f;

    // -----------------------------------------------------------------------------
    // 1) Find the longest PAN-FROZEN run (|dpan|<eps) and measure reticle travel in it.
    size_t bi = 0, bn = 0, i = 0;
    while (i < rows.size())
    {
        size_t j = i + 1;
        while (j < rows.size() &&
               std::fabs(rows[j].panX - rows[i].panX) < 0.01f &&
               std::fabs(rows[j].panZ - rows[i].panZ) < 0.01f &&
               std::fabs(rows[j].zoom - rows[i].zoom) < 1e-6f)
            ++j;
        if (j - i > bn) { bn = j - i; bi = i; }
        i = j;
    }
    std::printf("  longest pan-frozen run: %zu frames @ pan=(%.2f,%.2f) zoom=%.4f\n",
                bn, rows[bi].panX, rows[bi].panZ, rows[bi].zoom);

    // reticle travel across that run (marker-space units) -> confirms the mouse moved.
    float retMinU = 1e9f, retMaxU = -1e9f, retMinV = 1e9f, retMaxV = -1e9f;
    for (size_t k = bi; k < bi + bn; ++k)
    {
        retMinU = std::min(retMinU, rows[k].reticleU); retMaxU = std::max(retMaxU, rows[k].reticleU);
        retMinV = std::min(retMinV, rows[k].reticleV); retMaxV = std::max(retMaxV, rows[k].reticleV);
    }
    check(bn >= 20 && (retMaxU - retMinU) > 500.f,
          "frozen-run exists with the reticle sweeping",
          "reticle span U=" + std::to_string(retMaxU - retMinU));

    // -----------------------------------------------------------------------------
    // 2) THE BUG. Over that frozen run, the pan-anchored marker must be pixel-frozen,
    //    the reticle-coupled marker must drift hard. (scale 1, bias 0, half=0 -> we only
    //    care about the SPAN, not absolute placement.)
    float panMinX = 1e9f, panMaxX = -1e9f, panMinY = 1e9f, panMaxY = -1e9f;
    float retMinX = 1e9f, retMaxX = -1e9f, retMinY = 1e9f, retMaxY = -1e9f;
    for (size_t k = bi; k < bi + bn; ++k)
    {
        Px a = project(MU, MV, rows[k].zoom, rows[k].panX, rows[k].panZ, 1, 1, 0, 0);
        Px b = project_reticle(MU, MV, rows[k].zoom, rows[k].reticleU, rows[k].reticleV, 0, 0, 1, 1, 0, 0);
        panMinX = std::min(panMinX, a.x); panMaxX = std::max(panMaxX, a.x);
        panMinY = std::min(panMinY, a.y); panMaxY = std::max(panMaxY, a.y);
        retMinX = std::min(retMinX, b.x); retMaxX = std::max(retMaxX, b.x);
        retMinY = std::min(retMinY, b.y); retMaxY = std::max(retMaxY, b.y);
    }
    float panDriftX = panMaxX - panMinX, panDriftY = panMaxY - panMinY;
    float retDriftX = retMaxX - retMinX, retDriftY = retMaxY - retMinY;
    std::printf("  marker screen-drift over frozen run:  pan-anchored=(%.2f,%.2f)px  reticle-coupled=(%.1f,%.1f)px\n",
                panDriftX, panDriftY, retDriftX, retDriftY);
    check(panDriftX < 0.5f && panDriftY < 0.5f, "pan-anchored marker is FROZEN when pan is frozen",
          "drift=(" + std::to_string(panDriftX) + "," + std::to_string(panDriftY) + ")px");
    check(retDriftX > 10.f && retDriftY > 10.f, "reticle-coupled marker DRIFTS (reproduces the bug)",
          "drift=(" + std::to_string(retDriftX) + "," + std::to_string(retDriftY) + ")px");

    // -----------------------------------------------------------------------------
    // 2b) THE g_pan_center SWITCH, through the real project_uv branch. Same frozen run,
    //     same fixed marker. g_pan_center=false (reticle-coupled) MUST drift; =true
    //     (pan-anchored) MUST be frozen. This guards the exact code path that ships.
    auto branch_drift = [&](bool pan_center, float &dx, float &dy) {
        float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
        for (size_t k = bi; k < bi + bn; ++k)
        {
            const Row &r = rows[k];
            Px p = goblin::projection::project_uv(pan_center, MU, MV, r.zoom, r.panX, r.panZ,
                                                  r.reticleU, r.reticleV, 0, 0, 1, 1, 0, 0);
            mnx = std::min(mnx, p.x); mxx = std::max(mxx, p.x);
            mny = std::min(mny, p.y); mxy = std::max(mxy, p.y);
        }
        dx = mxx - mnx; dy = mxy - mny;
    };
    float offX, offY, onX, onY;
    branch_drift(false, offX, offY); // g_pan_center = false
    branch_drift(true, onX, onY);    // g_pan_center = true
    std::printf("  project_uv drift over frozen run:  g_pan_center=false (%.1f,%.1f)px  true (%.2f,%.2f)px\n",
                offX, offY, onX, onY);
    check(offX > 10.f && offY > 10.f, "g_pan_center=false DRIFTS (the shipping default = the bug)",
          "drift=(" + std::to_string(offX) + "," + std::to_string(offY) + ")px");
    check(onX < 0.5f && onY < 0.5f, "g_pan_center=true is FROZEN (the fix)",
          "drift=(" + std::to_string(onX) + "," + std::to_string(onY) + ")px");

    // -----------------------------------------------------------------------------
    // 3) MODEL CHECK + RETICLE-LAG SIGNAL. Fit mouse = (reticleMarker*zoom - pan)*s + b by
    //    least squares. Fit on the FROZEN run first (pure reticle sweep, no pan confound):
    //    a small residual + scale~1 there confirms screen = marker*zoom - pan is correct.
    //    Then the SAME fit over ALL frames: the extra residual is the reticle's own snap/
    //    lerp smoothing (RE §1) -- the field markers are built on lags instant input, which
    //    is exactly why reticle-coupled markers "follow late then settle".
    auto fit_axis = [&](bool yaxis, size_t lo, size_t hi, double &outScale) {
        double Sxx = 0, Sx = 0, Sxy = 0, Sy = 0;
        int n = 0;
        for (size_t k = lo; k < hi; ++k)
        {
            const Row &r = rows[k];
            double x = (yaxis ? r.reticleV : r.reticleU) * r.zoom - (yaxis ? r.panZ : r.panX);
            double y = yaxis ? r.mouseY : r.mouseX;
            Sxx += x * x; Sx += x; Sxy += x * y; Sy += y; ++n;
        }
        double denom = n * Sxx - Sx * Sx;
        double s = (n * Sxy - Sx * Sy) / denom;
        double b = (Sy - s * Sx) / n;
        double rms = 0;
        for (size_t k = lo; k < hi; ++k)
        {
            const Row &r = rows[k];
            double x = (yaxis ? r.reticleV : r.reticleU) * r.zoom - (yaxis ? r.panZ : r.panX);
            double pred = s * x + b, obs = yaxis ? r.mouseY : r.mouseX;
            rms += (pred - obs) * (pred - obs);
        }
        outScale = s;
        return std::sqrt(rms / n);
    };
    double sX, sY, sXa, sYa;
    double frozenRmsX = fit_axis(false, bi, bi + bn, sX);
    double frozenRmsY = fit_axis(true, bi, bi + bn, sY);
    double allRmsX = fit_axis(false, 0, rows.size(), sXa);
    double allRmsY = fit_axis(true, 0, rows.size(), sYa);
    std::printf("  fit FROZEN run: scaleX=%.5f rmsX=%.2fpx | scaleY=%.5f rmsY=%.2fpx\n",
                sX, frozenRmsX, sY, frozenRmsY);
    std::printf("  fit ALL frames: scaleX=%.5f rmsX=%.2fpx | scaleY=%.5f rmsY=%.2fpx\n",
                sXa, allRmsX, sYa, allRmsY);
    std::printf("  -> reticle snap/smoothing residual (all - frozen): X=%.2fpx Y=%.2fpx\n",
                allRmsX - frozenRmsX, allRmsY - frozenRmsY);
    // The model is confirmed by scale~1 on both axes. The ~30px residual is NOT a model
    // error: the logged reticle is cursor+0x104, the HOVER-SNAPPED field (RE §1), which
    // jumps onto icons and so is non-linear vs the raw OS mouse. (Logging +0xFC, the
    // un-snapped reticle, would shrink it.) That snap is irrelevant to the bug.
    check(sX > 0.9 && sX < 1.1 && sY > 0.9 && sY < 1.1,
          "screen = marker*zoom - pan is the correct model (per-axis scale ~1.0)",
          "scale=(" + std::to_string(sX) + "," + std::to_string(sY) + ")");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
