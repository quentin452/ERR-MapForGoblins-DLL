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

// ── IN-COMBAT state (docs/re/combat_state_gate_re_findings.md) ────────────────────────────────
// ER's per-entity battle state is the AI-FSM enum at [[ChrIns+0xC950]+0x30C] (state 6 = BATTLE; the
// player has no AI module so +0xC950 is null → skipped). "in combat" = ANY nearby enemy in state 6.
// Reuse the same CSFeMan entityHpBars[8] list the name feature walks (that IS ER's near-enemy set).
constexpr size_t kChrAiModule = 0xC950;  // ChrIns → AI think module (null for the player)
constexpr size_t kAiFsmState  = 0x30C;   // AI think module → FSM state int (6 = battle)
constexpr int    kFsmBattle   = 6;

bool any_enemy_in_battle_body(void **feman_slot, void **wcm_slot, GetChrInsFn getChr)
{
    if (!feman_slot || !wcm_slot || !getChr) return false;
    auto *feMan = *reinterpret_cast<uint8_t **>(feman_slot);
    if (!feMan) return false;
    void *wcm = *reinterpret_cast<void **>(wcm_slot);
    if (!wcm) return false;
    uint8_t *arr = feMan + kEntityArrOff;
    for (int i = 0; i < kEntityBars; ++i)
    {
        uint8_t *ent = arr + static_cast<size_t>(i) * kEntityStride;
        uint64_t handle = *reinterpret_cast<uint64_t *>(ent + kOffHandle);
        if (handle == kEmptyHandle) continue;
        uint64_t h = handle;
        void *chr = getChr(wcm, &h);
        if (!chr) continue;
        auto *ai = *reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(chr) + kChrAiModule);
        if (!ai) continue;  // player / no AI module
        if (*reinterpret_cast<int *>(ai + kAiFsmState) == kFsmBattle) return true;
    }
    return false;
}

bool any_enemy_in_battle_seh(void **feman_slot, void **wcm_slot, GetChrInsFn getChr)
{
    __try { return any_enemy_in_battle_body(feman_slot, wcm_slot, getChr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// DIAG: per-enemy-bar dump of the AI-module ptr + FSM-state candidates, so the combat offsets can be
// confirmed/corrected live (the map isn't closing in combat → combat_active() reads false). Writes into buf.
int combat_diag_body(void **feman_slot, void **wcm_slot, GetChrInsFn getChr, char *buf, int cap)
{
    int w = 0;
    auto app = [&](const char *fmt, auto... a) {
        if (w < cap - 1) w += std::snprintf(buf + w, cap - w, fmt, a...);
    };
    if (!feman_slot || !wcm_slot || !getChr) { app("unresolved slots"); return w; }
    auto *feMan = *reinterpret_cast<uint8_t **>(feman_slot);
    void *wcm = feMan ? *reinterpret_cast<void **>(wcm_slot) : nullptr;
    if (!feMan || !wcm) { app("feMan/wcm null"); return w; }
    uint8_t *arr = feMan + kEntityArrOff;
    int n = 0;
    for (int i = 0; i < kEntityBars; ++i)
    {
        uint8_t *ent = arr + static_cast<size_t>(i) * kEntityStride;
        uint64_t handle = *reinterpret_cast<uint64_t *>(ent + kOffHandle);
        if (handle == kEmptyHandle) continue;
        uint64_t h = handle;
        void *chr = getChr(wcm, &h);
        if (!chr) continue;
        auto *cb = reinterpret_cast<uint8_t *>(chr);
        int npc = *reinterpret_cast<int *>(cb + kChrOffNpc);
        void *ai = *reinterpret_cast<void **>(cb + kChrAiModule);
        int st = ai ? *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(ai) + kAiFsmState) : -999;
        app("[%d] npc=%d chr=%p ai(+0xC950)=%p fsm(+0x30C)=%d | ", n++, npc, chr, ai, st);
    }
    if (n == 0) app("no enemy bars");
    return w;
}

int combat_diag_seh(void **feman_slot, void **wcm_slot, GetChrInsFn getChr, char *buf, int cap)
{
    __try { return combat_diag_body(feman_slot, wcm_slot, getChr, buf, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { int n = std::snprintf(buf, cap, "SEH fault"); return n; }
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

// Category of a nameId==0 enemy we can name — drives the per-category name filter. teamType 24/27 is the
// hostile-NPC team (same signal used for the Hostile-NPC marker category); tier 3 is the vanilla
// field-boss / miniboss NpcName band; everything else is a regular mob.
//
// NB: per-category NAME COLOR was tried (inject an HTML <font color> around the name) and does NOT work —
// the native EnemyTag is force-recolored red by the engine's own setTextFormat AFTER our text is set, so
// an inline color is always overridden (every enemy/boss/invader tag is red by design). Coloring the tag
// would need a gfx edit of 01_000_fe.gfx, which is NOT mod-agnostic (conflicts with any HUD mod) — out of
// scope. So the category only gates the name FILTER; the string we feed is always the plain name.
enum class NameCat { Mob, FieldBoss, Hostile };
NameCat name_category(uint8_t team, int tier)
{
    if (team == 24 || team == 27) return NameCat::Hostile;
    if (tier == 3)                return NameCat::FieldBoss;
    return NameCat::Mob;
}
} // namespace

// Public: the enemy's display name (tiers 1-3, cached; "" = nameless). For the boss-marker
// enemy-supplement (map_entry_layer build_live_bosses) — mod-agnostic, reads the active install's
// NpcParam/NpcName (tier 3 = the vanilla field-boss band, e.g. "Erdtree Avatar").
std::string goblin::enemy_display_name(int npcParam, int model)
{
    return resolve_enemy_name(npcParam, model).name;
}

// Per-type record for the reconciler. Kept in a session-static map keyed by npcParamId.
namespace
{
struct TypeState
{
    int  kind = 0;          // 0 = nameable (we own it), 1 = engine already names it, 2 = truly nameless
    int32_t id = 0;         // reserved MFG NpcName id (stable across enable/disable); 0 until reserved
    bool applied = false;   // is NpcParam.nameId currently == id (i.e. WE wrote it)?
    bool injected = false;  // has the NpcName string for `id` been injected yet?
    NameCat cat = NameCat::Mob;
};

bool category_enabled(NameCat cat)
{
    if (!goblin::config::enemyNames) return false;  // master off -> nothing named
    switch (cat)
    {
        case NameCat::Hostile:   return goblin::config::nameEnemyHostiles;
        case NameCat::FieldBoss: return goblin::config::nameEnemyBosses;
        default:                 return goblin::config::nameEnemyMobs;
    }
}
} // namespace

// Native enemy names via the engine's OWN data path (docs/plans/native_enemy_names_scaleform_plan.md,
// docs/re/windows_enemy_name_hud_feed_re_findings.md). The engine renders the red EnemyTag name from
// NpcParam.nameId -> NpcName FMG and RE-READS nameId live, but only feeds the tag when the resolved
// name != "" -> nameId==0 generics stay blank. We drive that path for nameId==0 types OUR resolver can
// name: inject a NpcName string + set the type's NpcParam.nameId, so the engine renders our name in its
// own frame-synced, correctly-fonted tag (no ImGui overlay).
//
// RECONCILER (not fire-once): each frame, for every visible type, it compares the CURRENT settings
// (master + per-category filter + colorize/colors) against what we last applied and converges —
// applying (write id), REVERTING (write nameId=0), or RE-INJECTING (color changed) as needed. This is
// what makes the F1 toggles actually live: turning a category off un-names its enemies, turning it back
// on re-names them, flipping colorize recolors them — all without a reload. Must therefore run EVERY
// frame regardless of the master toggle (so it can revert when master is turned off). Present thread,
// host-side; per-type NpcName injects are batched into one FMG rebuild per frame.
// True if any nearby enemy (the CSFeMan HUD enemy-bar set) is in AI battle state — ER's own "in combat".
// Reuses the name-feature's resolved slots (resolve_once is idempotent). SEH-guarded; false on any read miss.
// Used to force-close the fullscreen vmap in combat, mirroring ER's native map-disable.
bool goblin::combat_active()
{
    resolve_once();
    // The precise AI-FSM state ([[ChrIns+0xC950]+0x30C]==6, combat_state_gate_re_findings.md) reads NULL on
    // the HP-bar ChrIns even for a normal enemy (live 2026-07-06) — the entityHpBars give a different ChrIns
    // than ER's WorldChrMan enemy list that the getter expects (precise path = a follow-up). Practical signal
    // that WORKS: any enemy HP bar present = engaged/in combat (bars appear when you fight; gone otherwise).
    // This must auto-close the vmap because the map key can't close it in combat (ER blocks the create-cb).
    if (!g_feman_slot || !g_wcm_slot || !g_get_chrins) return false;
    BarProbe pr;
    probe_bars_seh(g_feman_slot, g_wcm_slot, g_get_chrins, &pr);
    return pr.count > 0;
}

int goblin::combat_diag(char *buf, int cap)
{
    resolve_once();
    return combat_diag_seh(g_feman_slot, g_wcm_slot, g_get_chrins, buf, cap);
}

void goblin::update_native_enemy_names()
{
    resolve_once();
    if (!g_feman_slot || !g_wcm_slot || !g_get_chrins) return;

    BarProbe pr;
    probe_bars_seh(g_feman_slot, g_wcm_slot, g_get_chrins, &pr);
    if (pr.count <= 0) return;

    static std::unordered_map<int, TypeState> s_state;
    static int32_t s_next_id = kMfgNameIdBase;

    std::vector<goblin::FmgEntry> pending;                 // (re)injects this frame -> one FMG rebuild
    std::vector<std::pair<int, int32_t>> set_name;         // (npcParam, id)  write nameId = id
    std::vector<int> clear_name;                           // npcParam        write nameId = 0

    for (int i = 0; i < pr.count; ++i)
    {
        const int npcParam = pr.e[i].npcParam;
        if (npcParam <= 0) continue;

        TypeState &st = s_state[npcParam];

        // First sighting: classify the type (before we ever touch its nameId, so a live read is the
        // ORIGINAL value). nameId != 0 -> engine owns the tag; empty resolve -> nameless; else ours.
        if (st.kind == 0 && st.id == 0 && !st.injected && !st.applied)
        {
            uint8_t team = 0; int32_t nameId = 0;
            bool ok = goblin::npc_team_and_name((uint32_t)npcParam, &team, &nameId);
            if (ok && nameId != 0) { st.kind = 1; continue; }          // engine-named — leave alone
            const ResolvedName &rn = resolve_enemy_name(npcParam, pr.e[i].model);
            if (rn.name.empty()) { st.kind = 2; continue; }            // nameless — leave blank
            st.kind = 0;
            st.cat  = name_category(team, rn.tier);
            if (s_next_id >= kMfgNameIdMax)
            {
                spdlog::warn("[ENEMYBAR] MFG NpcName id band exhausted at {} — type {} stays unnamed",
                             s_next_id, npcParam);
                st.kind = 2;                                            // give up on this type
                continue;
            }
            st.id = s_next_id++;
        }
        if (st.kind != 0) continue;                                    // engine-owned / nameless

        // Reconcile against current settings.
        const bool want = category_enabled(st.cat);
        if (want)
        {
            if (!st.injected)                                          // inject the NpcName string once
            {
                const ResolvedName &rn = resolve_enemy_name(npcParam, pr.e[i].model);  // cached
                pending.push_back({st.id, utf8_to_wide(rn.name)});
                st.injected = true;
            }
            if (!st.applied) { set_name.emplace_back(npcParam, st.id); st.applied = true; }
        }
        else if (st.applied)                                           // disabled -> revert to vanilla
        {
            clear_name.push_back(npcParam);
            st.applied = false;
        }
    }

    // Inject all (re)injected NpcName strings in ONE FMG rebuild, THEN apply the param writes. The
    // engine re-reads nameId per tag-refresh, so the new value/string shows on the next refresh.
    if (!pending.empty() && !goblin::inject_fmg_entries(kNpcNameInjectSlot, pending))
    {
        spdlog::warn("[ENEMYBAR] NpcName inject failed for {} entries", pending.size());
        // Force a retry next frame for the affected types (their string didn't land).
        for (auto &e : pending)
            for (auto &kv : s_state)
                if (kv.second.id == e.id) kv.second.injected = false;
        return;
    }
    for (auto &pp : set_name)
        goblin::paramedit::param_set_field(L"NpcParam", (uint64_t)pp.first, 0x0c,
                                           goblin::paramedit::FieldType::S32, (double)pp.second);
    for (int npcParam : clear_name)
        goblin::paramedit::param_set_field(L"NpcParam", (uint64_t)npcParam, 0x0c,
                                           goblin::paramedit::FieldType::S32, 0.0);

    if (goblin::config::debugLogging && (!set_name.empty() || !clear_name.empty()))
        spdlog::info("[ENEMYBAR] reconcile: +{} named, -{} reverted, {} (re)injected",
                     set_name.size(), clear_name.size(), pending.size());
}
