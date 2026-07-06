// Mob NAMES on ELDEN RING's existing enemy health bar — via the engine's OWN native tag.
//
// The game draws the red enemy-name tag from NpcParam.nameId -> NpcName and re-reads nameId LIVE, but
// names only entities whose nameId != 0 (bosses / named NPCs) and leaves generics blank. Instead of
// drawing our OWN text (the old ImGui overlay — jittered on camera swings, edge-clamped, needed our
// font), we FEED the engine's path: walk the CSFeMan HUD manager's per-frame `entityHpBars[8]` array,
// turn each entry's entity handle into its live ChrIns, resolve the display name from the ACTIVE
// install (no bake), and for a type the engine leaves blank (nameId==0) inject a NpcName string + set
// its NpcParam.nameId -> the game renders the name in its own tag. RE: the writer is the vanilla
// engine, mechanism/offsets proven live 2026-07-06 (docs/re/windows_enemy_name_hud_feed_re_findings.md,
// docs/plans/native_enemy_names_scaleform_plan.md). Mod-agnostic — it IS the engine's own data path.
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
// THREADING/SAFETY: update_native_enemy_names() runs on the present thread (host side), single-
// threaded, no locks. The raw game-memory reads + the GetChrInsFromHandle game-function call run
// inside a __try with a noinline body (clang-cl elides __try around raw loads otherwise — see
// docs/memory/tooling/clang-cl-seh-noinline.md); a fault mid-teardown yields count=0. Name resolution
// (param table + FMG), the FMG inject, and the param write run OUTSIDE the SEH frame; the inject +
// param write happen at most once per npcParamId (cached), so the steady-state per-frame cost is the
// bar probe alone.

#include "goblin_inject.hpp"    // npc_team_and_name, update_native_enemy_names
#include "goblin_messages.hpp"  // lookup_text_utf8, raw_message_utf8, inject_fmg_entries, FmgEntry
#include "goblin_param_edit.hpp"// paramedit::param_set_field — write NpcParam.nameId live
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
#include <utility>
#include <vector>

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

// UTF-8 (our resolver's output) -> UTF-16 for the FMG injector (inject_fmg_entries wants wstring).
std::wstring utf8_to_wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// Reserved NpcName id band for MFG-injected generic names. Sits BELOW the tier-3 vanilla boss band
// (kBossBandBase 9e8 + model*1000) and ABOVE typical real NpcName ids, so an injected id collides with
// neither the active install's NpcName nor our own tier-3 boss lookups. One id per npcParamId, handed
// out sequentially, capped short of 9e8.
constexpr int32_t  kMfgNameIdBase     = 810000000;
constexpr int32_t  kMfgNameIdMax      = 899000000;  // stay under the 9e8 boss band
constexpr uint32_t kNpcNameInjectSlot = 18;         // base NpcName FMG slot the engine reads (RE-proven)

// Category of a nameId==0 enemy we can name — drives the per-category name filter + color. teamType
// 24/27 is the hostile-NPC team (same signal used for the Hostile-NPC marker category); tier 3 is the
// vanilla field-boss / miniboss NpcName band; everything else is a regular mob.
enum class NameCat { Mob, FieldBoss, Hostile };
NameCat name_category(uint8_t team, int tier)
{
    if (team == 24 || team == 27) return NameCat::Hostile;
    if (tier == 3)                return NameCat::FieldBoss;
    return NameCat::Mob;
}

// Return `s` iff it is a well-formed #RRGGBB hex, else "" — a malformed config color never corrupts
// the injected HTML (it just falls back to the plain, uncolored name).
std::string sanitize_hex(const std::string &s)
{
    if (s.size() != 7 || s[0] != '#') return {};
    for (size_t i = 1; i < 7; ++i)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return {};
    }
    return s;
}
} // namespace

// Public: the enemy's display name (tiers 1-3, cached; "" = nameless). For the boss-marker
// enemy-supplement (map_entry_layer build_live_bosses) — mod-agnostic, reads the active install's
// NpcParam/NpcName (tier 3 = the vanilla field-boss band, e.g. "Erdtree Avatar").
std::string goblin::enemy_display_name(int npcParam, int model)
{
    return resolve_enemy_name(npcParam, model).name;
}

// Native enemy names via the engine's OWN data path (docs/plans/native_enemy_names_scaleform_plan.md,
// docs/re/windows_enemy_name_hud_feed_re_findings.md). The engine renders the red EnemyTag name from
// NpcParam.nameId -> NpcName FMG and RE-READS nameId live, but only feeds the tag when the resolved
// name != "" -> nameId==0 generics stay blank. For each visible bar whose TYPE is nameId==0 yet OUR
// resolver can name (tiers 2/3), we inject a NpcName string + set that type's NpcParam.nameId, so the
// engine renders our name in its own frame-synced, correctly-fonted tag (no ImGui overlay -> no
// jitter/edge-clamp, accents free). Runs on the present thread, host-side. At most ONCE per npcParamId
// (s_assigned) and BATCHED per frame (one FMG rebuild), so the cost is a rare first-sighting hitch.
void goblin::update_native_enemy_names()
{
    resolve_once();
    if (!g_feman_slot || !g_wcm_slot || !g_get_chrins) return;

    BarProbe pr;
    probe_bars_seh(g_feman_slot, g_wcm_slot, g_get_chrins, &pr);
    if (pr.count <= 0) return;

    // npcParamId -> injected NpcName id. 0 = handled but NOT named (engine already names it, or truly
    // nameless) — never retried. A non-zero value = the id we injected for this type.
    static std::unordered_map<int, int32_t> s_assigned;
    static int32_t s_next_id = kMfgNameIdBase;

    std::vector<goblin::FmgEntry> pending;             // this frame's new NpcName strings (one rebuild)
    std::vector<std::pair<int, int32_t>> pending_param; // (npcParamId, id) written AFTER the inject lands

    for (int i = 0; i < pr.count; ++i)
    {
        const int npcParam = pr.e[i].npcParam;
        if (npcParam <= 0) continue;
        if (s_assigned.count(npcParam)) continue;      // already handled this TYPE

        // Engine already names it (nameId != 0 -> native tag shows)? Nothing to do; mark handled.
        uint8_t team = 0; int32_t nameId = 0;
        if (goblin::npc_team_and_name((uint32_t)npcParam, &team, &nameId) && nameId != 0)
        {
            s_assigned[npcParam] = 0;
            continue;
        }

        // nameId == 0: can WE name it (tiers 2/3)? Empty -> truly nameless, leave vanilla-blank.
        const ResolvedName &rn = resolve_enemy_name(npcParam, pr.e[i].model);
        if (rn.name.empty()) { s_assigned[npcParam] = 0; continue; }  // permanent skip (nothing to name)

        // Per-category name filter. Do NOT cache a filtered-out type: toggling its category back ON
        // should name it live next frame (resolve is cached, so the re-check is ~free).
        const NameCat cat = name_category(team, rn.tier);
        const bool want = (cat == NameCat::Hostile   && goblin::config::nameEnemyHostiles) ||
                          (cat == NameCat::FieldBoss && goblin::config::nameEnemyBosses)   ||
                          (cat == NameCat::Mob       && goblin::config::nameEnemyMobs);
        if (!want) continue;

        if (s_next_id >= kMfgNameIdMax)                // band exhausted (would take ~89M distinct types)
        {
            spdlog::warn("[ENEMYBAR] MFG NpcName id band exhausted at {} — remaining generics stay unnamed",
                         s_next_id);
            break;
        }
        int32_t id = s_next_id++;
        s_assigned[npcParam] = id;                     // marked now so a later frame won't re-queue it

        // The string the engine renders. Optional per-category HTML color (SPECULATIVE — only if the
        // tag's TextField parses inline HTML; off by default, malformed hex -> plain name).
        std::string display = rn.name;
        if (goblin::config::enemyNameColorize)
        {
            const std::string &raw = cat == NameCat::Hostile   ? goblin::config::enemyNameColorHostile
                                   : cat == NameCat::FieldBoss ? goblin::config::enemyNameColorBoss
                                                               : goblin::config::enemyNameColorMob;
            std::string hex = sanitize_hex(raw);
            if (!hex.empty())
                display = "<font color='" + hex + "'>" + rn.name + "</font>";
        }
        pending.push_back({id, utf8_to_wide(display)});
        pending_param.emplace_back(npcParam, id);

        if (goblin::config::debugLogging)
            spdlog::info("[ENEMYBAR] name '{}' -> NpcName[{}] npcParam={} model={} tier={} cat={} colored={}",
                         rn.name, id, npcParam, pr.e[i].model, rn.tier, (int)cat,
                         goblin::config::enemyNameColorize);
    }

    if (pending.empty()) return;

    // Inject all new NpcName strings in ONE FMG rebuild, THEN point each type's NpcParam.nameId (s32
    // @ +0x0c) at its id. The engine picks it up on the next tag-refresh (it re-reads nameId live).
    if (!goblin::inject_fmg_entries(kNpcNameInjectSlot, pending))
    {
        spdlog::warn("[ENEMYBAR] NpcName inject failed for {} entries", pending.size());
        return;  // types stay in s_assigned so we don't spam a failing inject every frame
    }
    for (auto &pp : pending_param)
        goblin::paramedit::param_set_field(L"NpcParam", (uint64_t)pp.first, 0x0c,
                                           goblin::paramedit::FieldType::S32, (double)pp.second);
}
