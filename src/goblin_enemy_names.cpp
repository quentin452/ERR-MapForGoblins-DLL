// Mob NAMES on ELDEN RING's existing enemy health bar.
//
// The game (vanilla) draws the enemy HP bar for every locked/aggroed enemy but shows the NAME only
// for bosses. This adds the name to the non-boss (entity) bars. We do NOT draw a bar and do NOT
// project world->screen: the engine already computed each on-screen HP bar and stored it in the
// CSFeMan HUD manager's per-frame `entityHpBars[8]` array. We read that array, turn each entry's
// entity handle into its NpcParam id, resolve the name via the existing mod-agnostic FMG path, and
// hand (screenPos, name) POD entries to the render side to draw over the bar.
//
// Struct offsets + signatures are derived from the bundled, WORKING PostureBarMod.dll (Mordrog) so
// they are live-valid on this ERR/ER build. Full recipe + credit:
//   docs/re/linux_enemy_healthbar_name_re_findings.md
//
// THREADING/SAFETY: get_enemy_bar_labels() is called from the render/present path (like
// get_player_world_pos). The raw game-memory reads AND the GetChrInsFromHandle game-function call run
// inside a __try with a noinline body (clang-cl elides __try around raw loads otherwise — see
// docs/memory/tooling/clang-cl-seh-noinline.md); a fault mid-teardown yields count=0. Name resolution
// (param table + FMG) runs OUTSIDE the SEH frame (it allocates).

#include "goblin_inject.hpp"    // EnemyBarLabel, npc_team_and_name
#include "goblin_messages.hpp"  // lookup_text_utf8
#include "goblin_config.hpp"    // config::debugLogging
#include "re_signatures.hpp"
#include "modutils.hpp"

#include <spdlog/spdlog.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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
constexpr uint64_t kEmptyHandle = 0xFFFFFFFFFFFFFFFFull;

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
    struct { float sx, sy; int npcParam; } e[kEntityBars];
};

// Raw derefs + the opaque GetChrInsFromHandle CALL live in a noinline body so clang-cl keeps the
// caller's __try (it elides __try wrapped directly around raw loads). count written incrementally;
// a mid-body fault leaves the partial count, which the SEH wrapper resets to 0.
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

        int npc = *reinterpret_cast<int *>(reinterpret_cast<uint8_t *>(chr) + kChrOffNpc);
        int k = pr->count;
        pr->e[k].sx = *reinterpret_cast<float *>(ent + kOffScreenX);
        pr->e[k].sy = *reinterpret_cast<float *>(ent + kOffScreenY);
        pr->e[k].npcParam = npc;
        pr->count = k + 1;
    }
}

void probe_bars_seh(void **feman_slot, void **wcm_slot, GetChrInsFn getChr, BarProbe *pr)
{
    pr->count = 0;
    __try { probe_bars_body(feman_slot, wcm_slot, getChr, pr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { pr->count = 0; }
}
} // namespace

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
        uint8_t team = 0;
        int32_t nameId = 0;
        if (!goblin::npc_team_and_name(static_cast<uint32_t>(pr.e[i].npcParam), &team, &nameId) ||
            nameId <= 0)
            continue;
        // NpcName FMG band = nameId + 700000000 (same resolve the quest/enemy-drop labels use).
        std::string nm = goblin::lookup_text_utf8(nameId + 700000000);
        if (nm.empty()) continue;
        buf[out].sx = pr.e[i].sx;
        buf[out].sy = pr.e[i].sy;
        std::snprintf(buf[out].name, sizeof(buf[out].name), "%s", nm.c_str());
        ++out;
    }

    if (goblin::config::debugLogging && (pr.count > 0 || out > 0))
        spdlog::info("[ENEMYBAR] visible={} named={}", pr.count, out);

    return out;
}
