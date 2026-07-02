// F1 panel — "Find item / object": marker search by name (game-language + English alias),
// results list with per-page rows, ring highlight + click-to-locate (cross-page switch +
// pan), the grace-discovery visited-page gate, and the dev locate-debug tree. Moved
// verbatim from goblin_overlay_render.cpp::draw_panel in the split (item 1).

#include "panel_internal.hpp"
#include "goblin_overlay_render_api.hpp"
#include "goblin_map_data.hpp"           // generated::Category
#include "goblin_worldmap_probe.hpp"     // LiveView
#include "goblin_bench.hpp"              // GOBLIN_BENCH (gate scan)
#include "worldmap/marker_layer.hpp"
#include "worldmap/map_renderer.hpp"     // set_item_search / locate_pending

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

namespace goblin::overlay::panel
{
void draw_item_search(const OverlayFrameCtx &ctx, Filter &f)
{
    // ── Find item / object ────────────────────────────────────────────────────────────
    // Search markers by NAME: type a fragment, get a results list; the matching markers are
    // ringed on the map (and pulled out of cluster piles). Click a result to point the map
    // cursor at it (cursor = the 2D map camera). The match set is rebuilt only when the query
    // changes (resolving ~thousands of FMG names per frame would be too costly), so the hot
    // render loop just does an O(1) name_id set lookup.
    // Hidden while the settings search filters it out; the last-applied ring/match
    // state simply persists (the query can't change while the block is hidden).
    if (!f.match("find item object search marker name locate ring quest"))
        return;

    ImGui::SeparatorText("Find item / object");
    // group bits (marker_layer): bit0 = underground, bit1 = DLC. Label the page so the
    // user knows where a hit is — locate only pans WITHIN the open page (cross-page needs
    // a manual page switch first; auto-switch would need page-transition RE).
    auto page_label = [](int g) -> const char * {
        switch (g & 3) { case 1: return "Underground"; case 2: return "DLC";
                         case 3: return "DLC Underground"; default: return "Overworld"; }
    };
    // One result row per (name, PAGE): an item on several pages (e.g. a Larval Tear on
    // Overworld + Underground + DLC) gets a separate row per page, so clicking a row locates
    // on THAT page. (Deduping by name_id alone collapsed them into one row carrying only the
    // first marker's group — the "shows only Underground" bug.)
    struct Hit { std::string label; int32_t name_id; int count; int group; bool quest = false; };
    static char item_q[64] = "";
    static std::string s_last_q;
    static std::unordered_set<int32_t> s_match;   // name_ids whose name matches (rendered ring)
    static std::vector<Hit> s_hits;               // deduped results for the list
    static int32_t s_pending_locate = 0;
    static std::string s_locate_label;            // clicked item name (pending banner)
    static int s_locate_group = 0;                // clicked item page (pending banner)

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##itemsearch", "find item/object by name... (e.g. larval, bolt of)",
                             item_q, sizeof(item_q));
    draw_gamepad_keyboard_button("##itemsearch_kbd", item_q, sizeof(item_q));

    // ── Locate debug (dev) — gated behind Verbose logging (debug_logging). Diagnoses the
    // item-search centring: LIVE view vs the target the engine should ease the pan toward
    // ("live-vs-target dPan" → CENTERED OK once converged). Kept for future locate issues.
    if ((*goblin::overlay_api::cfg_debugLogging_ptr()) && ImGui::TreeNode("Locate debug (dev)"))
    {
        goblin::worldmap_probe::LiveView dlv{};
        const bool dbg_open = goblin::overlay_api::get_live_view(dlv);
        const auto &d = goblin::overlay_api::last_locate_debug();
        if (dbg_open && dlv.zoom != 0.f)
        {
            const float cu = (dlv.panX + dlv.snapMidX) / dlv.zoom;
            const float cv = (dlv.panZ + dlv.snapMidZ) / dlv.zoom;
            ImGui::Text("LIVE pan=(%.1f, %.1f) zoom=%.3f  centre(marker)=(%.1f, %.1f)",
                        dlv.panX, dlv.panZ, dlv.zoom, cu, cv);
            ImGui::Text("LIVE snapMid=(%.1f, %.1f)", dlv.snapMidX, dlv.snapMidZ);
        }
        else
            ImGui::TextDisabled("map closed / no live view (open the world map)");
        ImGui::Separator();
        if (!d.ran)
            ImGui::TextDisabled("no locate yet — click a result");
        else
        {
            ImGui::Text("cursorOk=%d  wrote=%d  rectOk=%d", d.cursorOk, d.wrote, d.rectOk);
            ImGui::Text("req centre   = (%.1f, %.1f)", d.reqU, d.reqV);
            ImGui::TextColored(d.clamped ? ImVec4(1, 0.7f, 0.2f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                               "clamp centre = (%.1f, %.1f)  %s", d.clampU, d.clampV,
                               d.clamped ? "[CLAMPED -> near edge]" : "[no clamp]");
            ImGui::Text("zoom %.3f -> %.3f   world=[%.0f,%.0f .. %.0f,%.0f]",
                        d.zoomBefore, d.zoomUsed, d.wMinX, d.wMinZ, d.wMaxX, d.wMaxZ);
            ImGui::Text("pan TARGET   = (%.1f, %.1f)  (driven via cursor, not written)",
                        d.panWroteX, d.panWroteZ);
            if (dbg_open)
            {
                const float dx = dlv.panX - d.panWroteX, dz = dlv.panZ - d.panWroteZ;
                const bool off = (dx > 8.f || dx < -8.f || dz > 8.f || dz < -8.f);
                ImGui::TextColored(off ? ImVec4(1, 0.7f, 0.2f, 1) : ImVec4(0.3f, 1, 0.3f, 1),
                                   "live-vs-target dPan=(%.1f, %.1f)  %s", dx, dz,
                                   off ? "<- converging / not there yet" : "<- CENTERED OK");
            }
        }
        ImGui::TreePop();
    }

    if (s_last_q != item_q)
    {
        s_last_q = item_q;
        s_match.clear();
        s_hits.clear();
        if (item_q[0] != '\0')
        {
            // Resolve each distinct name_id once (cache), substring-match the query
            // against BOTH the live (game-language) label AND the bundled English
            // alias — so a player on a French/other game can type the English/wiki
            // name and still find it. The displayed label stays the game-language
            // name, with the English in parens when it differs (e.g. on a non-EN
            // game) to confirm the match.
            struct Names { std::string loc, en, label; };
            std::unordered_map<int32_t, Names> name_cache;
            // Dedup result rows by (name_id, page) via a composite key (name_id<<2 | group),
            // so each page an item is on becomes its own row. s_match stays keyed by name_id
            // so EVERY instance still highlights/rings on the map.
            std::map<int64_t, int> hit_count;  // (name_id<<2 | group) -> marker count
            // Keys (name<<2|page) that have at least ONE quest-NPC marker → badge the row
            // "[quest]" so the player can tell which search hit is a quest pin on the map
            // without clicking each one. OR'd across all markers of the key (a name_id could
            // appear as both a quest pin and something else).
            std::unordered_set<int64_t> quest_keys;
            const int questCat = static_cast<int>(goblin::generated::Category::WorldQuestNPC);
            for (auto *L : overlay_layers())
            {
                if (!L) continue;
                for (const auto &m : L->markers())
                {
                    if (m.name_id < 0) continue;
                    auto it = name_cache.find(m.name_id);
                    if (it == name_cache.end())
                    {
                        Names n;
                        n.loc = goblin::overlay_api::lookup_text_utf8(m.name_id);
                        // English alias resolved live from the active install's engus
                        // FMGs on disk (mod-agnostic; empty if unavailable → search
                        // degrades to game-language matching, no wrong-mod names).
                        n.en = goblin::overlay_api::lookup_name_en_disk_utf8(m.name_id);
                        // Label = game-language name; fall back to English if the
                        // live FMG had no entry. Append "(English)" only when it adds
                        // information (present and different from the shown name).
                        n.label = n.loc.empty() ? n.en : n.loc;
                        if (!n.en.empty() && n.en != n.label)
                            n.label += " (" + n.en + ")";
                        it = name_cache.emplace(m.name_id, std::move(n)).first;
                    }
                    const Names &nm = it->second;
                    if (nm.label.empty()) continue;
                    // Word-order-independent: each query token must appear in the
                    // combined game-language + English text (so "Claw Talisman" and
                    // "Talisman Claw" both match, and FR/EN words can be mixed).
                    if (!matches_all_tokens(nm.loc + " " + nm.en, item_q)) continue;
                    const int g = m.group & 3;
                    s_match.insert(m.name_id);                       // ring every instance
                    const int64_t k = ((int64_t)m.name_id << 2) | g; // per (name, page)
                    // One marker = one instance. Item stacking is now a non-destructive
                    // RENDER annotation (every co-located node stays a real marker in the
                    // bucket, reps + members alike), so a plain +1 per marker already gives
                    // the true on-map count (4 Formic Rock nodes read 4) regardless of the
                    // stack toggle — no per-stack adjustment needed.
                    const bool first = (hit_count[k] == 0);
                    hit_count[k] += 1;
                    if (m.category == questCat) quest_keys.insert(k);
                    if (first)
                        s_hits.push_back({nm.label, m.name_id, 0, g});
                }
            }
            for (auto &h : s_hits)
            {
                const int64_t k = ((int64_t)h.name_id << 2) | h.group;
                h.count = hit_count[k];
                h.quest = quest_keys.count(k) != 0;
            }
            // Group same-name rows together, pages in order (Overworld, Underground, DLC).
            std::sort(s_hits.begin(), s_hits.end(), [](const Hit &a, const Hit &b) {
                int c = a.label.compare(b.label);
                return c != 0 ? c < 0 : a.group < b.group;
            });
        }
    }

    if (item_q[0] != '\0')
    {
        // Locate (pan / page-switch) needs the world map OPEN — the live view + the
        // game-thread switch step only exist then. When closed, the list stays browsable
        // (what exists + which region) but the rows are disabled with a hint, so a click
        // can't leave a locate dangling forever.
        goblin::worldmap_probe::LiveView lv{};
        const bool map_open = goblin::overlay_api::get_live_view(lv);
        const int open_grp = map_open ? ((lv.openDlc ? 2 : 0) | (lv.underground ? 1 : 0)) : 0;

        if (map_open)
            ImGui::TextDisabled("%zu match%s (ringed on map; click = pan map onto it)",
                                s_hits.size(), s_hits.size() == 1 ? "" : "es");
        else if ((*goblin::overlay_api::cfg_showMinimap_ptr()))
            // <user> 2026-07-01: this used to always say "open the world map to
            // locate them" even with the minimap on — wrong, the minimap already
            // rings a hit (including off-range ones, clamped to the HUD edge).
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.f),
                               "%zu match%s (ringed on the minimap)",
                               s_hits.size(), s_hits.size() == 1 ? "" : "es");
        else
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
                               "%zu match%s - open the world map to locate them",
                               s_hits.size(), s_hits.size() == 1 ? "" : "es");

        // Region-visited gate from GRACE DISCOVERY (robust, save-backed — supersedes the
        // fragile dialog availability byte): if NO grace in a region has been rested at, the
        // player has never been there, so its map is undiscovered — grey those results +
        // don't teleport in. Covers DLC (bit1) AND underground (bit0). bit1=DLC, bit0=UG.
        // Recomputed LIVE every throttled tick (NOT latched) so DISCOVERING a new grace mid-
        // session unlocks its page within ~0.5s, and a save/character switch re-locks it.
        // There is NO reliable native O(1) "page discovered" flag (the dialog DLC byte
        // read unreliably; the UG flag was never found — see goblin_worldmap_probe.hpp
        // TODO(page_og_underground_available)). read_event_flag() IS O(1), though: harvest
        // the grace discover-flags per page group ONCE (the flag IDs are save-independent —
        // every grace ROW exists in BonfireWarpParam regardless of discovery), then each
        // throttled tick read only that small list.
        // DO NOT LATCH seen=true: these statics outlive the DLL, so a save/character switch
        // within one game session (graces un-discover) would keep a latched page wrongly
        // unlocked — the "I rested at no grace yet nothing blocks" bug. We recompute seen
        // LIVE every tick instead (the list is tiny, so it's free).
        static bool s_dlc_seen = false, s_ug_seen = false;
        static std::vector<uint32_t> s_dlc_grace_flags, s_ug_grace_flags;
        static bool s_grace_flags_built = false;
        static int s_visit_tick = 0;
        if (map_open && (++s_visit_tick % 30 == 0))
        {
            // Only fires during an active search (map open, 1 tick in 30). The first run
            // also builds the flag lists (one ~8477-marker pass); steady-state is just a
            // few-dozen flag reads — the bench line shows both.
            GOBLIN_BENCH("overlay.item_search.gate_scan");
            if (!s_grace_flags_built)
            {
                for (auto *L : overlay_layers())
                {
                    if (!L) continue;
                    for (const auto &m : L->markers())
                    {
                        if (!m.discover_flag) continue;  // graces only carry a discover flag
                        if (m.group & 2) s_dlc_grace_flags.push_back((uint32_t)m.discover_flag);
                        if (m.group & 1) s_ug_grace_flags.push_back((uint32_t)m.discover_flag);
                    }
                }
                s_grace_flags_built = true;
            }
            s_dlc_seen = false;
            for (uint32_t fl : s_dlc_grace_flags)
                if (goblin::overlay_api::read_event_flag(fl)) { s_dlc_seen = true; break; }
            s_ug_seen = false;
            for (uint32_t fl : s_ug_grace_flags)
                if (goblin::overlay_api::read_event_flag(fl)) { s_ug_seen = true; break; }
        }
        if (ImGui::BeginChild("##itemhits", ImVec2(0, 150), true))
        {
            if (!map_open) ImGui::BeginDisabled();
            for (size_t i = 0; i < s_hits.size(); i++)
            {
                const Hit &h = s_hits[i];
                const bool off_page = (h.group & 3) != (open_grp & 3);
                // Locked = this row's page is a region the player hasn't visited (no grace).
                const bool locked = map_open && (((h.group & 2) && !s_dlc_seen) ||
                                                 ((h.group & 1) && !s_ug_seen));
                char row[200];
                std::snprintf(row, sizeof(row), "%s  (x%d) - %s%s%s##h%zu", h.label.c_str(),
                              h.count, page_label(h.group),
                              h.quest ? " [quest]" : "",
                              locked ? " [undiscovered]" : "", i);
                if (locked) ImGui::BeginDisabled();
                if (ImGui::Selectable(row) && map_open)
                {
                    s_pending_locate = h.name_id;  // click → pan the map onto it
                    s_locate_label = h.label;      // remembered for the pending banner
                    s_locate_group = h.group;      // this row's page
                    ctx.nav_frames->store(90, std::memory_order_relaxed);  // wake the map so
                                      // the switch+pan apply with the F1 panel still open
                    // Cross-page: switch to this row's page+layer (overworld<->DLC +
                    // surface<->UG), marshalled onto the game thread, then the locate pans.
                    if (off_page)
                        goblin::overlay_api::request_switch_to_page(h.group);
                }
                if (locked) ImGui::EndDisabled();
                if (map_open && !locked && off_page && ImGui::IsItemHovered())
                    ImGui::SetTooltip("On the %s map — click to switch there + centre on it.",
                                      page_label(h.group));
                if (locked && ImGui::IsItemHovered())
                    ImGui::SetTooltip("On the %s map — you haven't discovered it yet.",
                                      page_label(h.group));
            }
            if (!map_open) ImGui::EndDisabled();
            if (s_hits.empty())
                ImGui::TextDisabled("no marker matches");
        }
        ImGui::EndChild();

        // Cross-page locate: the switch is marshalled to the game thread + the locate pans
        // the instant that page opens. The banner shows until it lands.
        if (map_open && goblin::worldmap::locate_pending())
            ImGui::TextColored(ImVec4(1.f, 0.85f, 0.2f, 1.f),
                               "> Locating \"%s\" on the %s map...", s_locate_label.c_str(),
                               page_label(s_locate_group));
    }
    // Hand the renderer the live match set + any pending locate (consumed once).
    goblin::worldmap::set_item_search(item_q[0] ? &s_match : nullptr, s_pending_locate);
    s_pending_locate = 0;
}
} // namespace goblin::overlay::panel
