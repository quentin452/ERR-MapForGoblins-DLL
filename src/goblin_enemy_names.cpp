// Mob NAMES on ELDEN RING's existing enemy health bar.
//
// The game draws the enemy HP bar for every locked/aggroed enemy but names only BOSSES; this adds
// the name to non-boss bars. We do NOT draw a bar and do NOT project world->screen: the engine
// already computed each on-screen HP bar and stored it in the CSFeMan HUD manager's per-frame
// `entityHpBars[8]` array. We read that array, turn each entry's entity handle into its live ChrIns,
// resolve the display name from the ACTIVE install's own regulation/msg files (no bake), and hand
// (screenPos, name) POD entries to the render side to draw over the bar.
//
// Struct offsets + signatures are derived from the bundled, WORKING PostureBarMod.dll (Mordrog) so
// they are live-valid on this ERR/ER build. The layered NAME resolution is the Windows-RE result in
// docs/re/windows_enemy_name_runtime_source_re_findings.md (mod-agnostic, no table):
//   tier 1  NpcParam.nameId -> NpcName        (named entities: invaders/NPCs/some minibosses)
//   tier 2  TutorialTitle bestiary codex      (id = model*1000 + variant*100 + {10,4}; ERR + any mod
//                                               with a codex names EVERY generic enemy)
//   tier 3  NpcName boss band 9e8+model*1000  (vanilla field bosses whose nameId is 0, e.g. Tree
//                                               Sentinel 903251600 — EMEVD HandleBossHealthBar ids)
//   else    nameless (vanilla-correct for a true generic) — draw nothing.
//
// THREADING/SAFETY: get_enemy_bar_labels() runs on the render/present path (like get_player_world_pos)
// — single-threaded, no locks. The raw game-memory reads + the GetChrInsFromHandle game-function call
// run inside a __try with a noinline body (clang-cl elides __try around raw loads otherwise — see
// docs/memory/tooling/clang-cl-seh-noinline.md); a fault mid-teardown yields count=0. Name resolution
// (param table + FMG) runs OUTSIDE the SEH frame and is cached per npcParamId.

#include "goblin_inject.hpp"    // EnemyBarLabel, npc_team_and_name
#include "goblin_messages.hpp"  // lookup_text_utf8, raw_message_utf8
#include "goblin_config.hpp"    // config::debugLogging
#include "re_signatures.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
// GetChrInsFromHandle(WorldChrMan* wcm, uint64_t* handlePtr) -> ChrIns*.
using GetChrInsFn = void *(*)(void *wcm, uint64_t *handlePtr);

// CSFeManImp layout (PostureBarMod-derived, this ER build):
constexpr size_t kEntityArrOff = 0x59F0;  // EntityHpBar entityHpBars[8]
constexpr size_t kEntityStride = 0x40;    // sizeof(EntityHpBar)
constexpr int    kEntityBars   = 8;       // ENTITY_CHR_ARRAY_LEN
constexpr size_t kOffHandle    = 0x00;    // u64 entityHandle
constexpr size_t kOffScreenX   = 0x10;    // float screenPosX
constexpr size_t kOffScreenY   = 0x14;    // float screenPosY
constexpr size_t kOffVisible   = 0x34;    // bool isVisible
constexpr size_t kChrOffNpc    = 0x60;    // ChrIns.npcParam (npcParamId)
constexpr size_t kChrOffModel  = 0x64;    // ChrIns.modelNumber (e.g. 3251 for c3251)
constexpr uint64_t kEmptyHandle = 0xFFFFFFFFFFFFFFFFull;

// FMG bands / slots for the name resolution (see decode_textid in goblin_messages.cpp).
constexpr int32_t kNpcNameBand   = 700000000;  // nameId + this -> NpcName (tier 1)
constexpr int32_t kTutorialBand  = 900000000;  // id + this -> TutorialTitle (tier 2)
constexpr int32_t kBossBandBase  = 900000000;  // NpcName boss band = this + model*1000 + suffix (t3)
const uint32_t    kNpcNameSlots[] = {428, 328, 18}; // NpcName physical FMG slots (base/dlc/fallback)

void **g_feman_slot = nullptr;  // &(CSFeManImp*)
void **g_wcm_slot   = nullptr;  // &(WorldChrMan*)
GetChrInsFn g_get_chrins = nullptr;
bool g_tried = false;

void resolve_once()
{
    if (g_tried) return;
    g_tried = true;

    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));

    // WorldChrMan slot: doc-confirmed fixed RVA preferred, WCM_FINDER AOB as patch-drift fallback
    // (mirrors resolve_world_chr_man in goblin_world_position.cpp).
    void **wcm_fixed = er ? reinterpret_cast<void **>(er + 0x3D65F88) : nullptr;
    void **wcm_aob = nullptr;
    if (auto *finder = reinterpret_cast<uint8_t *>(
            modutils::scan<void>({.aob = goblin::sig::WCM_FINDER})))
    {
        int32_t disp = *reinterpret_cast<int32_t *>(finder + 0xA);
        wcm_aob = reinterpret_cast<void **>(finder + 0xE + disp);
    }
    g_wcm_slot = wcm_fixed ? wcm_fixed : wcm_aob;

    // CSFeManImp slot: mov rcx,[rip+disp] (disp @+3, instr len 7).
    g_feman_slot = reinterpret_cast<void **>(
        modutils::scan<void *>({.aob = goblin::sig::CSFEMAN_SLOT, .relative_offsets = {{3, 7}}}));

    // GetChrInsFromHandle: matched address IS the function.
    g_get_chrins = reinterpret_cast<GetChrInsFn>(
        modutils::scan<void>({.aob = goblin::sig::GET_CHRINS_FROM_HANDLE}));

    spdlog::info("[ENEMYBAR] resolve er=0x{:X} CSFeMan_slot={:p} WCM_slot={:p} GetChrInsFromHandle={:p}",
                 er, (void *)g_feman_slot, (void *)g_wcm_slot, (void *)g_get_chrins);
}

// POD snapshot filled inside the SEH frame; name resolution happens after, outside it.
struct BarProbe
{
    int count;
    struct { float sx, sy; int npcParam, model; uint64_t handle; } e[kEntityBars];
};

// Raw derefs + the opaque GetChrInsFromHandle CALL live in a noinline body so clang-cl keeps the
// caller's __try. count written incrementally; a mid-body fault leaves a partial count the SEH
// wrapper resets to 0.
__declspec(noinline) void probe_bars_body(void **feman_slot, void **wcm_slot, GetChrInsFn getChr,
                                          BarProbe *pr)
{
    pr->count = 0;
    auto *feMan = *reinterpret_cast<uint8_t **>(feman_slot);
    if (!feMan) return;
    void *wcm = *reinterpret_cast<void **>(wcm_slot);
    if (!wcm) return;

    uint8_t *arr = feMan + kEntityArrOff;
    for (int i = 0; i < kEntityBars; ++i)
    {
        uint8_t *ent = arr + static_cast<size_t>(i) * kEntityStride;
        uint64_t handle = *reinterpret_cast<uint64_t *>(ent + kOffHandle);
        if (handle == kEmptyHandle) continue;
        if (!*reinterpret_cast<bool *>(ent + kOffVisible)) continue;

        uint64_t h = handle;                 // GetChrInsFromHandle takes a u64* (by pointer)
        void *chr = getChr(wcm, &h);
        if (!chr) continue;

        auto *cb = reinterpret_cast<uint8_t *>(chr);
        int k = pr->count;
        pr->e[k].sx = *reinterpret_cast<float *>(ent + kOffScreenX);
        pr->e[k].sy = *reinterpret_cast<float *>(ent + kOffScreenY);
        pr->e[k].npcParam = *reinterpret_cast<int *>(cb + kChrOffNpc);
        pr->e[k].model    = *reinterpret_cast<int *>(cb + kChrOffModel);
        pr->e[k].handle   = handle;
        pr->count = k + 1;
    }
}

void probe_bars_seh(void **feman_slot, void **wcm_slot, GetChrInsFn getChr, BarProbe *pr)
{
    pr->count = 0;
    __try { probe_bars_body(feman_slot, wcm_slot, getChr, pr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pr->count = 0; }
}

// Strip the codex-entry prefix from a TutorialTitle bestiary name: "116. Tree Sentinel" ->
// "Tree Sentinel", "172a. Troll" -> "Troll". Pattern: ^\d+[a-z]?\.\s*
std::string strip_codex_prefix(const std::string &s)
{
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == 0) return s;                                   // no leading number → not a codex title
    if (i < s.size() && s[i] >= 'a' && s[i] <= 'z') ++i;    // optional variant letter
    if (i >= s.size() || s[i] != '.') return s;             // must be "<num>[a]."
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// Resolve the display name for an enemy from the ACTIVE install (tiers 1-3; "" = nameless). Cached
// per npcParamId — computed at most once per distinct enemy param (tier 3 does a 1000-wide FMG scan
// on first sight of a vanilla boss, so caching matters).
struct ResolvedName { std::string name; int tier; };

const ResolvedName &resolve_enemy_name(int npcParam, int model)
{
    static std::unordered_map<int, ResolvedName> cache;
    auto it = cache.find(npcParam);
    if (it != cache.end()) return it->second;

    std::string name;
    int tier = 0;

    // Tier 1: NpcParam.nameId -> NpcName.
    uint8_t team = 0; int32_t nameId = 0;
    if (goblin::npc_team_and_name(static_cast<uint32_t>(npcParam), &team, &nameId) && nameId > 0)
    {
        name = goblin::lookup_text_utf8(nameId + kNpcNameBand);
        if (!name.empty()) tier = 1;
    }

    // Tier 2: TutorialTitle bestiary codex (id = model*1000 + variant*100 + {10,4}; variant then
    // variant-0 fallback since codex sub-entries don't always match the param variant digit).
    if (name.empty() && model > 0)
    {
        long variant = (static_cast<long>(npcParam) % 10000) / 1000;
        long bases[] = { static_cast<long>(model) * 1000 + variant * 100,
                         static_cast<long>(model) * 1000 };
        const int suffixes[] = {10, 4};
        for (long base : bases)
        {
            for (int suf : suffixes)
            {
                long id = kTutorialBand + base + suf;
                if (id <= 0 || id > 0x7fffffff) continue;
                std::string t = goblin::lookup_text_utf8(static_cast<int32_t>(id));
                if (!t.empty()) { name = strip_codex_prefix(t); tier = 2; break; }
            }
            if (!name.empty()) break;
        }
    }

    // Tier 3: NpcName boss band (9e8 + model*1000 + suffix 0..999) — vanilla field bosses / minibosses
    // whose NpcParam.nameId is 0. Read raw on the NpcName slots (the id is already the full FMG id, so
    // it must NOT go through the decode_textid band router). First non-empty wins.
    if (name.empty() && model > 0)
    {
        long modelBase = kBossBandBase + static_cast<long>(model) * 1000;
        for (int suffix = 0; suffix < 1000 && name.empty(); ++suffix)
        {
            long id = modelBase + suffix;
            if (id <= 0 || id > 0x7fffffff) break;
            for (uint32_t slot : kNpcNameSlots)
            {
                std::string t = goblin::raw_message_utf8(slot, static_cast<uint32_t>(id));
                if (!t.empty()) { name = t; tier = 3; break; }
            }
        }
    }

    auto [ins, _] = cache.emplace(npcParam, ResolvedName{std::move(name), tier});
    return ins->second;
}
} // namespace

// Public: the enemy's display name (tiers 1-3, cached; "" = nameless). For the boss-marker
// enemy-supplement (map_entry_layer build_live_bosses) — mod-agnostic, reads the active install's
// NpcParam/NpcName (tier 3 = the vanilla field-boss band, e.g. "Erdtree Avatar").
std::string goblin::enemy_display_name(int npcParam, int model)
{
    return resolve_enemy_name(npcParam, model).name;
}

// On-screen thresholds (1920x1080 space, PostureBarMod values): keep the label inside a sane band so
// it doesn't ride the extreme screen edge or fall off the bottom (raw screenPos clamps to 1080 there).
constexpr float kClampL = 130.0f, kClampR = 1790.0f, kClampT = 175.0f, kClampB = 990.0f;
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int goblin::get_enemy_bar_labels(EnemyBarLabel *buf, int max)
{
    if (!buf || max <= 0) return 0;
    resolve_once();
    if (!g_feman_slot || !g_wcm_slot || !g_get_chrins) return 0;

    BarProbe pr;
    probe_bars_seh(g_feman_slot, g_wcm_slot, g_get_chrins, &pr);

    int out = 0;
    for (int i = 0; i < pr.count && out < max; ++i)
    {
        const ResolvedName &rn = resolve_enemy_name(pr.e[i].npcParam, pr.e[i].model);
        if (rn.name.empty()) continue;  // true generic with no name in the active install → draw nothing
        // Draw at the raw bar position (it's smooth frame-to-frame — no extrapolation needed), clamped
        // to the on-screen band so the name stays put instead of riding the edge.
        buf[out].sx = clampf(pr.e[i].sx, kClampL, kClampR);
        buf[out].sy = clampf(pr.e[i].sy, kClampT, kClampB);
        std::snprintf(buf[out].name, sizeof(buf[out].name), "%s", rn.name.c_str());
        ++out;
    }

    if (goblin::config::debugLogging && pr.count > 0)
        for (int i = 0; i < pr.count; ++i)
        {
            const ResolvedName &rn = resolve_enemy_name(pr.e[i].npcParam, pr.e[i].model);
            spdlog::info("[ENEMYBAR] vis={} named={} | [{}] npc={} model={} tier={} name='{}'",
                         pr.count, out, i, pr.e[i].npcParam, pr.e[i].model, rn.tier, rn.name);
        }

    return out;
}
