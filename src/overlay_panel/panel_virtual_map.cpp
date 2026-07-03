// F1 panel — MapForGoblins VIRTUAL WORLD MAP (mod-owned page, slice A: the pannable/zoomable canvas).
//
// The strategic call (docs/re/worldmap_new_page_spike_findings.md): a NATIVE new worldmap page is an
// unsolved engine WRITE frontier, but the overlay already owns its own backbuffer drawing — so a custom
// "dev world" map is a MOD-OWNED virtual page we draw ourselves, no engine registration. This is the
// container for it: an ImGui window with a world-space canvas (drag = pan, wheel = zoom about the cursor),
// a reference grid + origin cross, and a mod-defined world→canvas projection. Markers on it come next
// (slice B); a real bundle-backed custom world is World Virtualization vision #1.
//
// Independent of the game's Scaleform map — this draws whenever the overlay is active and the window is
// open (toggled from the Dev tab). Gamepad/keyboard nav works via the already-enabled ImGui nav.

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"
#include "worldmap/marker_layer.hpp"   // Marker / MarkerLayer (overlay_layers → markers to project)

#include <cmath>
#include <cstdio>

namespace goblin::overlay::panel
{
using goblin::i18n::tr;

namespace
{
    // View state (world-space). cam = the world XZ at the canvas centre; zoom = pixels per world unit.
    // Persisted across frames; the whole map lives in ER world units (same space as marker world pos).
    bool s_open = false;
    float s_cam_x = 0.0f, s_cam_z = 0.0f;
    float s_zoom = 0.05f;  // 0.05 px/unit → ~10k-unit ER map spans ~525px; a sane default overview
    constexpr float kZoomMin = 0.002f, kZoomMax = 4.0f;
    // Which marker group to lay out on the canvas (slice B uses the live mod markers as test data;
    // slice C ties markers to a custom world). group = isDLC*2|isUG → 0/1/2/3.
    int s_group = 0;
    bool s_fit_requested = false;  // one-shot: on next draw, frame the selected group's markers
    int s_drawn = 0;               // marker count drawn last frame (toolbar readout)
    const char *const kGroupNames[4] = {"Base overworld", "Base underground", "DLC overworld",
                                        "DLC underground"};
}

bool &virtual_map_open() { return s_open; }
void virtual_map_request_fit() { s_fit_requested = true; }
void virtual_map_set_group(int g) { if (g >= 0 && g < 4) s_group = g; }
int virtual_map_group() { return s_group; }

void draw_virtual_map(const OverlayFrameCtx & /*ctx*/)
{
    if (!s_open) return;

    ImGui::SetNextWindowSize(ImVec2(720.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(470.0f, 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("MapForGoblins \xE2\x80\x94 Virtual World Map (WIP)"), &s_open))
    {
        ImGui::End();
        return;
    }

    // Toolbar: reset view + group selector + fit-to-markers + readout.
    if (ImGui::SmallButton(tr("Reset view")))
    {
        s_cam_x = s_cam_z = 0.0f;
        s_zoom = 0.05f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Fit"))) s_fit_requested = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo(tr("group"), tr(kGroupNames[s_group])))
    {
        for (int i = 0; i < 4; i++)
            if (ImGui::Selectable(tr(kGroupNames[i]), s_group == i)) s_group = i;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled(tr("drag = pan   wheel = zoom   |   centre (%.0f, %.0f)  zoom %.3f px/u   markers %d"),
                        s_cam_x, s_cam_z, s_zoom, s_drawn);

    // Canvas = the remaining content region. An InvisibleButton captures drag/scroll over exactly it.
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 32.0f) size.x = 32.0f;
    if (size.y < 32.0f) size.y = 32.0f;
    const ImVec2 canvas_end(origin.x + size.x, origin.y + size.y);
    const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);

    ImGui::InvisibleButton("##vmap_canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO &io = ImGui::GetIO();

    // world → screen and screen → world (mod-defined projection; Z maps to screen-Y, no flip needed for
    // a mod page — we own the convention). Keep them inverse so zoom-about-cursor is exact.
    auto w2s = [&](float wx, float wz) {
        return ImVec2(center.x + (wx - s_cam_x) * s_zoom, center.y + (wz - s_cam_z) * s_zoom);
    };
    auto s2w = [&](ImVec2 s, float &wx, float &wz) {
        wx = s_cam_x + (s.x - center.x) / s_zoom;
        wz = s_cam_z + (s.y - center.y) / s_zoom;
    };

    // Pan: dragging moves the camera opposite the mouse delta (in world units).
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
    {
        s_cam_x -= io.MouseDelta.x / s_zoom;
        s_cam_z -= io.MouseDelta.y / s_zoom;
    }
    // Zoom about the cursor: keep the world point under the mouse fixed across the zoom step.
    if (hovered && io.MouseWheel != 0.0f)
    {
        float wx_before, wz_before;
        s2w(io.MousePos, wx_before, wz_before);
        s_zoom *= std::pow(1.2f, io.MouseWheel);
        if (s_zoom < kZoomMin) s_zoom = kZoomMin;
        if (s_zoom > kZoomMax) s_zoom = kZoomMax;
        float wx_after, wz_after;
        s2w(io.MousePos, wx_after, wz_after);
        s_cam_x += wx_before - wx_after;  // re-centre so the cursor stays over the same world point
        s_cam_z += wz_before - wz_after;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, canvas_end, true);
    dl->AddRectFilled(origin, canvas_end, IM_COL32(18, 20, 26, 255));  // dark canvas backdrop

    // Reference grid: pick a world spacing that renders ~80px on screen, snapped to a 1/2/5×10^n step,
    // so grid density stays readable across the whole zoom range.
    const float target_px = 80.0f;
    float raw = target_px / s_zoom;                 // world units per ~80px
    float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    float norm = raw / mag;
    float step = (norm < 1.5f ? 1.0f : norm < 3.5f ? 2.0f : norm < 7.5f ? 5.0f : 10.0f) * mag;

    float wx0, wz0, wx1, wz1;
    s2w(origin, wx0, wz0);
    s2w(canvas_end, wx1, wz1);
    const ImU32 grid_col = IM_COL32(60, 66, 78, 255);
    const ImU32 axis_col = IM_COL32(120, 130, 150, 255);
    for (float gx = std::ceil(wx0 / step) * step; gx <= wx1; gx += step)
    {
        ImVec2 a = w2s(gx, wz0), b = w2s(gx, wz1);
        dl->AddLine(ImVec2(a.x, origin.y), ImVec2(a.x, canvas_end.y),
                    std::fabs(gx) < step * 0.5f ? axis_col : grid_col);
    }
    for (float gz = std::ceil(wz0 / step) * step; gz <= wz1; gz += step)
    {
        ImVec2 a = w2s(wx0, gz), b = w2s(wx1, gz);
        dl->AddLine(ImVec2(origin.x, a.y), ImVec2(canvas_end.x, a.y),
                    std::fabs(gz) < step * 0.5f ? axis_col : grid_col);
    }

    // Markers (slice B): project the selected group's live mod markers onto the canvas + accumulate a
    // bbox for Fit. A colored dot per marker (m.color); on-canvas icons are a follow-up. This uses the
    // live ER markers as test data — slice C ties markers to a bundle-backed custom world.
    int drawn = 0;
    float minx = 1e30f, minz = 1e30f, maxx = -1e30f, maxz = -1e30f;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const goblin::worldmap::Marker &m : L->markers())
        {
            if (m.group != s_group) continue;
            if (m.worldX < minx) minx = m.worldX;
            if (m.worldX > maxx) maxx = m.worldX;
            if (m.worldZ < minz) minz = m.worldZ;
            if (m.worldZ > maxz) maxz = m.worldZ;
            ImVec2 ps = w2s(m.worldX, m.worldZ);
            if (ps.x < origin.x || ps.x > canvas_end.x || ps.y < origin.y || ps.y > canvas_end.y) continue;
            dl->AddCircleFilled(ps, 3.0f, m.color ? m.color : IM_COL32(235, 130, 90, 255));
            drawn++;
        }
    }
    s_drawn = drawn;
    // Fit: frame the selected group's bbox (cam/zoom feed w2s next frame → 1-frame settle).
    if (s_fit_requested && maxx > minx && maxz > minz)
    {
        s_cam_x = (minx + maxx) * 0.5f;
        s_cam_z = (minz + maxz) * 0.5f;
        float zx = size.x * 0.9f / (maxx - minx), zz = size.y * 0.9f / (maxz - minz);
        s_zoom = zx < zz ? zx : zz;
        if (s_zoom < kZoomMin) s_zoom = kZoomMin;
        if (s_zoom > kZoomMax) s_zoom = kZoomMax;
    }
    s_fit_requested = false;

    // World origin cross (0,0) + a small label, so the empty canvas is legible.
    ImVec2 o = w2s(0.0f, 0.0f);
    if (o.x >= origin.x && o.x <= canvas_end.x && o.y >= origin.y && o.y <= canvas_end.y)
    {
        dl->AddCircleFilled(o, 4.0f, IM_COL32(220, 180, 90, 255));
        dl->AddText(ImVec2(o.x + 6, o.y + 4), IM_COL32(220, 180, 90, 255), "0,0");
    }
    // Grid-step legend (bottom-left) so the scale is readable.
    char legend[64];
    std::snprintf(legend, sizeof(legend), "%s: %.0f u", tr("grid"), step);
    dl->AddText(ImVec2(origin.x + 6, canvas_end.y - 18), IM_COL32(150, 158, 172, 255), legend);

    // TODO(slice B): project the mod's markers for the virtual world's group here (w2s per marker + the
    // marker icon draw), then (slice C) tag markers to a synthetic group / bundle-backed custom world.
    dl->PopClipRect();

    ImGui::End();
}
} // namespace goblin::overlay::panel
