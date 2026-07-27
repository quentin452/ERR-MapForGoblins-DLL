// F1 panel — "Run": a run / completion tracker (deaths, in-game time, boss checklist).
//
// Why it lives HERE and not in a second mod: the two numbers come from the save's own
// GameDataMan block (goblin::inventory::read_run_stats) and the boss list is the WorldBosses
// marker bucket this mod already builds LIVE (build_live_bosses — WorldMapPointParam rows plus
// the mod-agnostic seed from the game's own boss health bar). A standalone tracker would have to
// carry a baked vanilla boss list to show the same panel, which is wrong on any mod; here the
// data is already correct for whatever install is loaded. See docs/memory/features/run-tracker.md.
//
// The "defeated" state is the marker's clearedEventFlagId, which now has TWO sources: the
// WorldMapPointParam row (ERR-style installs) and, for bosses seeded mod-agnostically, the
// engine's own 2003[12] defeat registration mined from the active install's EMEVD — where the
// flag id IS the boss entity id. A boss with neither still lands here with flag 0; those are
// counted separately as "state unknown" rather than silently reported as alive.

#include "panel_internal.hpp"
#include "goblin_i18n.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_config.hpp"             // run HUD placement / toggle
#include "goblin_inventory.hpp"          // read_run_stats (host-side, GameDataMan)
#include "goblin_map_data.hpp"           // generated::Category
#include "goblin_inject.hpp"             // boss_bar_display_name (tier 4 from an entity id)
#include "worldmap/marker_layer.hpp"
#include "worldmap/loot_disk.hpp"        // emevd_boss_defeat_entities — the fight list

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace goblin::overlay::panel
{
using goblin::i18n::tr;

namespace
{
// One boss ENCOUNTER. Identity is the cleared event flag, not the marker: a boss type can own
// several markers (multi-instance types like the Erdtree Avatars, or a multi-part arena split
// across tiles), but the game sets ONE flag when that encounter is beaten. Keying on the flag is
// what makes killed/total mean "fights", the same unit a player counts.
struct BossRow
{
    int flag = 0;
    std::string name;
    std::string region;
    bool defeated = false;
};

struct RunSnapshot
{
    std::vector<BossRow> bosses;  // sorted by region, then name
    int killed = 0;
    int unflagged = 0;            // registered fights with no map marker (no region header)
    bool built = false;
    // The game's event-flag API answers false for EVERY id until it is warm (before the world is
    // up, and on a cold API right after load). Without this, a run with 200 bosses beaten reads
    // 0/201 — indistinguishable from a fresh character. Probed with flag 6001 ("AlwaysOn"), the
    // liveness convention refresh_quest_finishable already uses.
    bool flags_live = false;
    double last_refresh = -1.0;
    // Last good save counters — read_run_stats fails on the title/loading screen, and showing 0
    // there would look like a wiped run. Keep what we last saw and say it's stale instead.
    uint32_t deaths = 0, igt_ms = 0;
    bool stats_ok = false;
};

RunSnapshot &snap()
{
    static RunSnapshot s;
    return s;
}

// H:MM:SS once the run passes an hour, MM:SS below — a fresh character reading "0:04:12" is
// noise. Same shape the community trackers use, so a screenshot is comparable.
std::string format_igt(uint32_t ms)
{
    const unsigned total = ms / 1000u;
    const unsigned h = total / 3600u, m = (total % 3600u) / 60u, s = total % 60u;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
    else       std::snprintf(buf, sizeof(buf), "%02u:%02u", m, s);
    return buf;
}

// Display name for the HUD key. Only the reverse direction is missing from the config layer
// (goblin::parse_vk_code parses name -> VK, nothing maps back), and the binding is edited in the
// INI, so this stays a local label: F-keys by name, anything else as its raw VK.
std::string key_label(uint32_t vk)
{
    char buf[16];
    if (vk >= 0x70 && vk <= 0x87) std::snprintf(buf, sizeof(buf), "F%u", vk - 0x70 + 1);
    else                          std::snprintf(buf, sizeof(buf), "VK 0x%02X", vk);
    return buf;
}

// Rebuild the encounter set from the live marker layers. Walks every marker once and hits the
// event-flag reader once per DISTINCT flag, so it is O(markers) with ~one flag read per fight —
// fine on demand, too heavy for every frame (the map can hold >1000 boss markers), hence the
// 1 s throttle at the call site.
void rebuild()
{
    RunSnapshot &s = snap();
    s.flags_live = goblin::overlay_api::read_event_flag(6001);  // AlwaysOn — is the flag API warm?

    // The MARKERS are only an enrichment source here (name + region). The fight LIST comes from the
    // EMEVD defeat registrations, because a marker is a map-display object: build_live_bosses dedups
    // it per (type, tile) and drops LOD-tile-only / non-MSB-Enemy placements, all correct for drawing
    // and all wrong for counting. Sourcing the list from markers lost 14 of 215 fights on the test
    // install (Malenia among them) — see docs/memory/features/run-tracker.md.
    const int bossCat = static_cast<int>(goblin::generated::Category::WorldBosses);
    struct MarkerInfo { std::string name, region; };
    std::unordered_map<int, MarkerInfo> by_flag;
    for (auto *L : overlay_layers())
    {
        if (!L) continue;
        for (const auto &m : L->markers())
        {
            if (m.category != bossCat || m.cleared_flag == 0) continue;
            if (by_flag.count(m.cleared_flag)) continue;  // another instance of the same fight
            MarkerInfo mi;
            // Mod-agnostic bosses carry their resolved name inline (no FMG id — see
            // Marker::live_name); everything else resolves through the active install's FMGs.
            mi.name = !m.live_name.empty() ? m.live_name
                                           : goblin::overlay_api::lookup_text_utf8(m.name_id);
            if (m.loc_pname >= 0) mi.region = goblin::overlay_api::lookup_text_utf8(m.loc_pname);
            by_flag.emplace(m.cleared_flag, std::move(mi));
        }
    }

    std::vector<BossRow> rows;
    int unflagged = 0;                 // registered fights with no marker to name/place them
    std::unordered_map<int, size_t> seen;  // flag -> row: two entities can share one fight's flag
    for (const auto &kv : goblin::worldmap::emevd_boss_defeats())
    {
        const uint32_t entity = kv.first, flag = kv.second;
        auto dup = seen.find((int)flag);
        if (dup != seen.end())
        {
            // Same fight, second body (or a re-registration): keep the row, just fill in a name if
            // this entity resolves one and the first did not.
            if (rows[dup->second].name.empty())
                rows[dup->second].name = goblin::boss_bar_display_name(entity);
            continue;
        }
        BossRow r;
        r.flag = (int)flag;
        auto it = by_flag.find(r.flag);
        if (it != by_flag.end()) { r.name = it->second.name; r.region = it->second.region; }
        else ++unflagged;
        // No marker (or a marker with no name): the boss-bar name resolves from the ENTITY, which
        // is not always the flag — hence looking it up with `entity`, not `r.flag`.
        if (r.name.empty()) r.name = goblin::boss_bar_display_name(entity);
        r.defeated = s.flags_live && goblin::overlay_api::read_event_flag(flag);
        seen.emplace(r.flag, rows.size());
        rows.push_back(std::move(r));
    }
    for (BossRow &r : rows)
        if (r.name.empty()) r.name = tr("(unnamed boss)");

    std::sort(rows.begin(), rows.end(), [](const BossRow &a, const BossRow &b) {
        if (a.region != b.region) return a.region < b.region;
        return a.name < b.name;
    });
    s.killed = 0;
    for (const BossRow &r : rows)
        if (r.defeated) ++s.killed;
    s.bosses = std::move(rows);
    s.unflagged = unflagged;
    // Only a rebuild that could actually READ flags counts as built; otherwise the next tick
    // retries instead of latching a cold-API 0.
    s.built = s.flags_live;
    s.last_refresh = ImGui::GetTime();
    // One line per state change — enough to diagnose a 0/N from the log alone (is the flag API
    // cold, or has the player genuinely beaten nothing?) without flooding the 1 s tick.
    static int s_last_logged = -1;
    if (s.killed != s_last_logged)
    {
        s_last_logged = s.killed;
        spdlog::info("[RUN] bosses {}/{} defeated ({} registered fights have no map marker, "
                     "flag API {})", s.killed, (int)s.bosses.size(), s.unflagged,
                     s.flags_live ? "live" : "COLD - counts are not readable yet");
    }
}
}  // namespace

// In-game HUD: one compact CLICK-THROUGH line (NoInputs — it must never eat a click or a
// gamepad focus while you are fighting), on its own key. This is the in-game surface; the F1
// tab below is the detailed one. Same corner/anchor idiom as the minimap HUD.
void draw_run_hud_window()
{
    if (!goblin::config::runHud) return;

    RunSnapshot &s = snap();
    uint32_t d = 0, t = 0;
    if (goblin::inventory::read_run_stats(d, t))
    {
        s.deaths = d;
        s.igt_ms = t;
        s.stats_ok = true;
    }
    if (!s.stats_ok) return;  // title screen / loading — draw nothing rather than zeros

    // The boss count reuses the panel's snapshot and its 1 s throttle, but the HUD must NOT
    // trigger the rebuild walk on its own every second while the player is just walking around:
    // it shows whatever the last rebuild produced (0/0 until the map/panel built it once).
    if (goblin::config::runHudBosses && !s.built && ImGui::GetTime() - s.last_refresh > 5.0)
        rebuild();

    char line[160];
    // Boss part only once the flag API is warm — a cold "Bosses 0/201" on the HUD would be a lie
    // at a glance, and the HUD has no room to explain itself. It appears on its own a moment later.
    if (goblin::config::runHudBosses && s.flags_live && !s.bosses.empty())
        std::snprintf(line, sizeof(line), "%s %u   %s   %s %d/%d", tr("Deaths"), s.deaths,
                      format_igt(s.igt_ms).c_str(), tr("Bosses"), s.killed,
                      static_cast<int>(s.bosses.size()));
    else
        std::snprintf(line, sizeof(line), "%s %u   %s", tr("Deaths"), s.deaths,
                      format_igt(s.igt_ms).c_str());

    const ImGuiIO &io = ImGui::GetIO();
    const ImVec2 pad = ImGui::GetStyle().WindowPadding;
    const ImVec2 sz = ImGui::CalcTextSize(line);
    const float w = sz.x + pad.x * 2.0f, h = sz.y + pad.y * 2.0f;
    const float x = goblin::config::runHudAnchorRight
                        ? io.DisplaySize.x - w - goblin::config::runHudOffsetX
                        : goblin::config::runHudOffsetX;
    const float y = goblin::config::runHudAnchorBottom
                        ? io.DisplaySize.y - h - goblin::config::runHudOffsetY
                        : goblin::config::runHudOffsetY;

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(goblin::config::runHudOpacity);
    // NoInputs is the whole point of this surface: click-through, no nav focus, no cursor grab.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##mfg_run_hud", nullptr, flags))
        ImGui::TextUnformatted(line);
    ImGui::End();
}

void draw_run_tracker(Filter &f)
{
    if (!f.match("run speedrun tracker deaths death counter igt in-game time playtime "
                 "boss bosses defeated progress completion checklist"))
        return;

    RunSnapshot &s = snap();

    ImGui::SeparatorText(tr("Run"));

    // Save counters — 2 guarded reads off the cached GameDataMan slot; cheap enough per frame,
    // and only while this section is actually drawn.
    uint32_t d = 0, t = 0;
    const bool live = goblin::inventory::read_run_stats(d, t);
    if (live)
    {
        s.deaths = d;
        s.igt_ms = t;
        s.stats_ok = true;
    }

    if (!s.stats_ok)
    {
        ImGui::TextDisabled("%s", tr("No save loaded yet (title screen / loading)."));
    }
    else
    {
        ImGui::Text("%s: %u", tr("Deaths"), s.deaths);
        ImGui::SameLine(0.0f, 24.0f);
        ImGui::Text("%s: %s", tr("In-game time"), format_igt(s.igt_ms).c_str());
        if (!live)
            ImGui::TextDisabled("%s", tr("(last known values — the game data is not readable "
                                         "right now)"));
    }

    // Boss progress. Refreshed on a 1 s tick while the section is visible so a kill shows up
    // without reopening the panel, and on demand from the button.
    const double now = ImGui::GetTime();
    if (!s.built || now - s.last_refresh > 1.0) rebuild();

    const int total = static_cast<int>(s.bosses.size());
    if (!s.flags_live)
    {
        // Never print 0/N off a cold flag API — that reads as "you have beaten nothing".
        ImGui::Text("%s: -/%d", tr("Bosses defeated"), total);
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::TextDisabled("%s", tr("(the game's event flags are not readable yet)"));
    }
    else
    {
        ImGui::Text("%s: %d/%d", tr("Bosses defeated"), s.killed, total);
        if (total > 0)
        {
            ImGui::SameLine(0.0f, 24.0f);
            ImGui::Text("(%.0f%%)",
                        100.0f * static_cast<float>(s.killed) / static_cast<float>(total));
        }
    }
    ImGui::SameLine(0.0f, 24.0f);
    if (ImGui::SmallButton(tr("Refresh"))) rebuild();

    if (total == 0)
    {
        ImGui::TextDisabled("%s", tr("The install's boss events have not been read yet — open the "
                                     "map once so the EMEVD scan runs."));
    }
    else if (s.unflagged > 0)
    {
        // Counted, just not placeable: the fight is registered by the engine, but no map marker
        // survived for it, so it has no region header. Informational, not a warning.
        ImGui::TextDisabled("%d %s", s.unflagged,
                            tr("of them have no map marker (counted, but not placed on the map)."));
    }

    // In-game HUD controls. The HUD, not this tab, is the surface meant for actual play — F1
    // takes the cursor and covers the screen, so it is the wrong place to watch a timer.
    ImGui::Separator();
    ImGui::Checkbox(tr("Show the compact HUD in game"), &goblin::config::runHud);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s: %s)", tr("key"), key_label(goblin::config::runHudKey).c_str());
    if (goblin::config::runHud)
    {
        ImGui::Checkbox(tr("Include the boss count in the HUD"), &goblin::config::runHudBosses);
        ImGui::Checkbox(tr("HUD: anchor to the right edge"), &goblin::config::runHudAnchorRight);
        ImGui::SameLine();
        ImGui::Checkbox(tr("bottom edge"), &goblin::config::runHudAnchorBottom);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat(tr("HUD opacity"), &goblin::config::runHudOpacity, 0.0f, 1.0f, "%.2f");
    }

    if (total > 0 && ImGui::TreeNode(tr("Boss checklist")))
    {
        // Region headers, in the sort order built above. The list can be long (200+ fights), so
        // it lives in its own scroll region rather than stretching the panel.
        ImGui::BeginChild("##runbosses", ImVec2(0.0f, 320.0f), true);
        std::string current;
        bool first = true;
        for (const BossRow &r : s.bosses)
        {
            if (first || r.region != current)
            {
                current = r.region;
                first = false;
                ImGui::SeparatorText(current.empty() ? tr("(no region)") : current.c_str());
            }
            // Read-only state: this mirrors the save, it does not edit it. (EROverlay offers a
            // "revive" that clears the flag — a save write we deliberately do not do here.)
            if (r.defeated) ImGui::TextUnformatted("[x]");
            else            ImGui::TextDisabled("[ ]");
            ImGui::SameLine();
            if (r.defeated) ImGui::TextUnformatted(r.name.c_str());
            else            ImGui::TextDisabled("%s", r.name.c_str());
        }
        ImGui::EndChild();
        ImGui::TreePop();
    }
}
}  // namespace goblin::overlay::panel
