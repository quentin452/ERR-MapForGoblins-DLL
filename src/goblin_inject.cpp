#include "goblin_inject.hpp"
#include "goblin_collected.hpp"
#include "goblin_kindling.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "generated/goblin_map_data.hpp"
#include "from/params.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using ParamRowInfo = from::params::ParamRowInfo;
using ParamTable = from::params::ParamTable;
using ParamResCap = from::params::ParamResCap;
using Category = goblin::generated::Category;

static void *allocation = nullptr;

// State for runtime toggle (ERSC-hosting workaround). On hotkey press the
// pointers stored on the param-res-cap are swapped between vanilla and
// expanded values. set_param_injection_active() is a no-op until
// inject_map_entries() has populated these.
static uint8_t **g_file_ptr_ref = nullptr;
static int64_t *g_file_size_ref = nullptr;
static uint8_t *g_vanilla_param_file = nullptr;
static int64_t g_vanilla_param_size = 0;
static uint8_t *g_expanded_param_file = nullptr;
static int64_t g_expanded_param_size = 0;
static bool g_param_injection_active = false;

// Master-off intent set by the toggle hotkey. When true the user has
// explicitly hidden the icons, so the auto-toggle must keep the table vanilla
// even while the world map is open. Shared between the hotkey and watcher
// threads; a lone bool flag is fine, but use atomic for correctness.
static std::atomic<bool> g_icons_user_disabled{false};

// EXPERIMENT: toast-method cycler. F10 fires a toast with the current method;
// F11 cycles the method. Lets us A/B every text-injectable notification path
// in-game. Remove once the final style is chosen.
// Default = method 1 (trampoline). User confirmed this is the codex-style
// upper-left plaque we want. Methods 0/2/3 retained for A/B testing via F11.
static std::atomic<int> g_toast_method{1};
static const char *const TOAST_METHOD_NAMES[] = {
    "0 Summon p=1 fp=1 u=1 (narrow plaque just below center, ~5s)",
    "1 ShowTutorialPopup trampoline (AOB-resolved; codex upper-left, default)",
};
// TutorialParam row ids exposed in goblin_inject.hpp. These are NEW rows
// injected by inject_tutorial_popup_rows() with textId pointing at
// TutorialBody.fmg entries injected by goblin_messages — so the upper-left
// codex toast renders our text without modifying any vanilla/ERR data.
static constexpr int TOAST_METHOD_COUNT =
    (int)(sizeof(TOAST_METHOD_NAMES) / sizeof(TOAST_METHOD_NAMES[0]));

struct WrapperRowLocator
{
    int32_t row;
    int32_t index;
};

static ParamResCap *find_world_map_point_param_res_cap()
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view name = from::params::dlw_c_str(&prc->param_name);
        if (name == L"WorldMapPointParam") return prc;
    }
    return nullptr;
}

static bool is_category_enabled(Category cat)
{
    switch (cat)
    {
    case Category::EquipArmaments:       return goblin::config::showArmaments;
    case Category::EquipArmour:          return goblin::config::showArmour;
    case Category::EquipAshesOfWar:      return goblin::config::showAshesOfWar;
    case Category::EquipSpirits:         return goblin::config::showSpirits;
    case Category::EquipTalismans:       return goblin::config::showTalismans;
    case Category::KeyCelestialDew:      return goblin::config::showCelestialDew;
    case Category::KeyCookbooks:         return goblin::config::showCookbooks;
    case Category::KeyCrystalTears:      return goblin::config::showCrystalTears;
    case Category::KeyImbuedSwordKeys:   return goblin::config::showImbuedSwordKeys;
    case Category::KeyLarvalTears:       return goblin::config::showLarvalTears;
    case Category::KeyScadutreeFragments: return goblin::config::showScadutreeFragments;
    case Category::KeyGreatRunes:        return goblin::config::showGreatRunes;
    case Category::KeyLostAshes:         return goblin::config::showLostAshes;
    case Category::KeyPotsNPerfumes:     return goblin::config::showPotsNPerfumes;
    case Category::KeySeedsTears:        return goblin::config::showSeedsTears;
    case Category::KeyWhetblades:        return goblin::config::showWhetblades;
    case Category::LootAmmo:             return goblin::config::showAmmo;
    case Category::LootBellBearings:     return goblin::config::showBellBearings;
    case Category::LootMerchantBellBearings: return goblin::config::showMerchantBellBearings;
    case Category::LootConsumables:      return goblin::config::showConsumables;
    case Category::LootCraftingMaterials:return goblin::config::showCraftingMaterials;
    case Category::LootMPFingers:        return goblin::config::showMPFingers;
    case Category::LootMaterialNodes:    return goblin::config::showMaterialNodes;
    case Category::LootReusables:        return goblin::config::showReusables;
    case Category::LootSmithingStones:       return goblin::config::showSmithingStones;
    case Category::LootSmithingStonesLow:   return goblin::config::showSmithingStonesLow;
    case Category::LootSmithingStonesRare:  return goblin::config::showSmithingStonesRare;
    case Category::LootGoldenRunes:         return goblin::config::showGoldenRunes;
    case Category::LootGoldenRunesLow:      return goblin::config::showGoldenRunesLow;
    case Category::LootStoneswordKeys:   return goblin::config::showStoneswordKeys;
    case Category::LootThrowables:       return goblin::config::showThrowables;
    case Category::LootPrattlingPates:   return goblin::config::showPrattlingPates;
    case Category::LootRuneArcs:         return goblin::config::showRuneArcs;
    case Category::LootDragonHearts:     return goblin::config::showDragonHearts;
    case Category::LootGloveworts:       return goblin::config::showGloveworts;
    case Category::LootGreatGloveworts:  return goblin::config::showGreatGloveworts;
    case Category::LootRadaFruit:        return goblin::config::showRadaFruit;
    case Category::LootGestures:         return goblin::config::showGestures;
    case Category::LootGreases:          return goblin::config::showGreases;
    case Category::LootUtilities:        return goblin::config::showUtilities;
    case Category::LootStatBoosts:       return goblin::config::showStatBoosts;
    case Category::ReforgedFortunes:     return goblin::config::showFortunes;
    case Category::WorldHostileNPC:      return goblin::config::showHostileNPC;
    case Category::MagicIncantations:    return goblin::config::showIncantations;
    case Category::MagicMemoryStones:    return goblin::config::showMemoryStones;
    case Category::MagicPrayerbooks:     return goblin::config::showPrayerbooks;
    case Category::MagicSorceries:       return goblin::config::showSorceries;
    case Category::WorldBosses:          return goblin::config::showBosses;
    case Category::QuestDeathroot:       return goblin::config::showDeathroot;
    case Category::QuestProgression:     return goblin::config::showProgression;
    case Category::QuestSeedbedCurses:   return goblin::config::showSeedbedCurses;
    case Category::ReforgedEmberPieces:  return goblin::config::showEmberPieces;
    case Category::ReforgedItemsAndChanges: return goblin::config::showItemsAndChanges;
    case Category::ReforgedRunePieces:   return goblin::config::showRunePieces;
    case Category::WorldGraces:          return goblin::config::showGraces;
    case Category::WorldImpStatues:      return goblin::config::showImpStatues;
    case Category::WorldMaps:            return goblin::config::showWorldMaps;
    case Category::WorldPaintings:       return goblin::config::showPaintings;
    case Category::WorldSpiritSprings:   return goblin::config::showSpiritSprings;
    case Category::WorldSpiritspringHawks: return goblin::config::showSpiritspringHawks;
    case Category::WorldStakesOfMarika:  return goblin::config::showStakesOfMarika;
    case Category::WorldSummoningPools:  return goblin::config::showSummoningPools;
    case Category::WorldKindlingSpirits: return goblin::config::showKindlingSpirits;
    case Category::WorldInteractables:   return goblin::config::showInteractables;
    default:                             return true;
    }
}

void goblin::inject_map_entries()
{
    // (The CSFreeListMemorySystem int3-assert NOP patch that used to run here
    // was removed 2026-05-29: it was an artifact of the old hosting-crash
    // theory. The real cause was the 16-align bug in the wrapper_row_locator
    // layout; with that fixed, hosting works with no assert patching —
    // verified live. See docs/ersc_hosting_and_map_autohide.md.)

    struct InjectedEntry
    {
        int32_t row_id;
        uint64_t original_row_id;
        const from::paramdef::WORLD_MAP_POINT_PARAM_ST *data;
        bool is_piece;     // collected::register_param_ptr (CSWorldGeomMan-tracked)
        bool is_kindling;  // kindling::register_param_ptr  (SFX-region-tracked)
        Category category;
    };

    // Filter: only include enabled categories (disabled ones are simply not injected)
    std::vector<InjectedEntry> entries;
    entries.reserve(generated::MAP_ENTRY_COUNT);

    size_t skipped_by_config = 0;
    for (size_t i = 0; i < generated::MAP_ENTRY_COUNT; i++)
    {
        const auto &e = generated::MAP_ENTRIES[i];
        if (!is_category_enabled(e.category))
        {
            skipped_by_config++;
            continue;
        }
        bool is_piece = e.category == Category::ReforgedRunePieces ||
                        e.category == Category::ReforgedEmberPieces ||
                        e.category == Category::LootMaterialNodes;
        bool is_kindling = e.category == Category::WorldKindlingSpirits;
        entries.push_back({0, e.row_id, &e.data, is_piece, is_kindling, e.category});
    }

    spdlog::info("Injecting {} map entries ({} skipped by config)",
                 entries.size(), skipped_by_config);

    auto param_res_cap = find_world_map_point_param_res_cap();
    if (!param_res_cap)
    {
        spdlog::error("WorldMapPointParam not found");
        return;
    }

    auto *rescap = reinterpret_cast<uint8_t *>(param_res_cap->param_header);
    auto *&file_ptr_ref = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size_ref = *reinterpret_cast<int64_t *>(rescap + 0x78);

    auto *old_param_file = file_ptr_ref;
    auto *old_table = reinterpret_cast<ParamTable *>(old_param_file);
    uint16_t orig_num_rows = old_table->num_rows;

    spdlog::debug("Original WorldMapPointParam: {} rows", orig_num_rows);

    // Collect vanilla row IDs to avoid collisions
    std::set<int32_t> vanilla_ids;
    for (uint16_t i = 0; i < orig_num_rows; i++)
        vanilla_ids.insert(static_cast<int32_t>(old_table->rows[i].row_id));

    // Assign sequential IDs starting from 1, skipping vanilla IDs
    std::unordered_map<uint64_t, uint64_t> id_remap;  // original -> dynamic
    int32_t next_id = 1;
    for (auto &entry : entries)
    {
        while (vanilla_ids.count(next_id))
            next_id++;
        id_remap[entry.original_row_id] = static_cast<uint64_t>(next_id);
        entry.row_id = next_id++;
    }

    // Update collected + kindling systems with new dynamic IDs
    collected::remap_row_ids(id_remap);
    kindling::remap_row_ids(id_remap);

    spdlog::debug("Assigned IDs: {} entries, range {}-{}, remapped {} piece IDs",
                  entries.size(),
                  entries.empty() ? 0 : entries.front().row_id,
                  entries.empty() ? 0 : entries.back().row_id,
                  id_remap.size());

    uint32_t new_entry_count = static_cast<uint32_t>(entries.size());
    uint32_t total_rows = orig_num_rows + new_entry_count;

    spdlog::debug("Injecting {} entries ({} total)", new_entry_count, total_rows);

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    constexpr size_t ROW_LOCATOR_SIZE = sizeof(ParamRowInfo);
    constexpr size_t PARAM_DATA_SIZE = sizeof(from::paramdef::WORLD_MAP_POINT_PARAM_ST);
    constexpr size_t WRAPPER_ROW_LOC_SIZE = sizeof(WrapperRowLocator);

    const char *type_str = reinterpret_cast<const char *>(old_param_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * ROW_LOCATOR_SIZE;
    size_t data_end = data_start + total_rows * PARAM_DATA_SIZE;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    // Align wrapper_row_loc to 16: the param lookup-by-id engine reads this
    // offset from the wrapper header and rounds it UP to 16 (`(x+0xf)&~0xf`)
    // before using it as the binary-search base. 4-align worked for WMP only
    // because it's iterated, never id-looked-up — but keep it correct so an
    // id lookup (or a future engine path) can't read past the array. (This
    // exact bug crashed TutorialParam save-load; see inject_tutorial_popup_rows.)
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    // HeapAlloc (not VirtualAlloc): Seamless Co-op's `game_memory_unlimiter`
    // module crashes when hosting if our expanded ParamTable lives on a
    // dedicated VirtualAlloc'd page region — ERSC apparently expects param
    // memory to come from the process heap. HEAP_ZERO_MEMORY zero-inits.
    allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("HeapAlloc failed ({} bytes)", total_alloc);
        return;
    }

    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_param_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<ParamTable *>(new_param_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = static_cast<uint32_t>(wrapper_row_loc_start);
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = static_cast<int32_t>(total_rows);

    memcpy(new_param_file, old_param_file, HEADER_SIZE);
    new_table->num_rows = static_cast<uint16_t>(total_rows);
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_param_file + 0x00) = static_cast<uint32_t>(type_str_start);
    *reinterpret_cast<uint16_t *>(new_param_file + 0x04) = static_cast<uint16_t>(data_start);
    *reinterpret_cast<uint64_t *>(new_param_file + 0x30) = data_start;

    memcpy(new_param_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
        bool is_piece;
        bool is_kindling;
        Category category;
    };

    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);

    for (uint16_t i = 0; i < orig_num_rows; i++)
    {
        auto *data = old_param_file + old_table->rows[i].param_offset;
        all_rows.push_back({static_cast<int32_t>(old_table->rows[i].row_id), data, false, false, {}});
    }
    for (auto &entry : entries)
    {
        all_rows.push_back({entry.row_id, reinterpret_cast<const uint8_t *>(entry.data),
                            entry.is_piece, entry.is_kindling, entry.category});
    }

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_param_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_param_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        new_locators[i].row_id = static_cast<uint64_t>(all_rows[i].row_id);
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_param_file + data_offset, all_rows[i].data_ptr, PARAM_DATA_SIZE);
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = static_cast<int32_t>(i);

        // Boss/hawk kill display mode: green checkmark vs hide killed
        auto cat = all_rows[i].category;
        if (cat == Category::WorldBosses || cat == Category::WorldSpiritspringHawks)
        {
            auto *p = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(
                new_param_file + data_offset);
            if (goblin::config::hideKilledBosses)
            {
                p->clearedEventFlagId = 0;  // no green checkmark, text hides → icon hides
            }
            else
            {
                p->textDisableFlagId1 = 0;  // keep green checkmark, don't hide text
                p->textDisableFlagId2 = 0;  // keep location text visible too
            }
        }
    }

    // Register Rune/Ember piece + kindling-spirit pointers for real-time tracking.
    // Pieces are CSWorldGeomMan-driven (collected::); kindling spirits are
    // SFX-region-driven (kindling::). Same hide-trick (areaNo = 99).
    int registered_pieces = 0, hidden_pieces = 0;
    int registered_kindling = 0, hidden_kindling = 0;
    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * PARAM_DATA_SIZE;
        auto *param_ptr = new_param_file + data_offset;
        uint64_t row_id = static_cast<uint64_t>(all_rows[i].row_id);

        if (all_rows[i].is_piece)
        {
            collected::register_param_ptr(row_id, param_ptr);
            registered_pieces++;
            if (collected::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;  // areaNo = 99
                hidden_pieces++;
            }
        }
        else if (all_rows[i].is_kindling)
        {
            kindling::register_param_ptr(row_id, param_ptr);
            registered_kindling++;
            if (kindling::is_row_collected(row_id))
            {
                param_ptr[0x20] = 99;
                hidden_kindling++;
            }
        }
    }

    spdlog::info("Registered {} piece + {} kindling pointers ({} + {} hidden at inject)",
                 registered_pieces, registered_kindling, hidden_pieces, hidden_kindling);

    spdlog::debug("Swapping param_file pointer: {:p} -> {:p}", (void *)old_param_file, (void *)new_param_file);

    // Capture state for runtime toggle. Save original size before overwriting.
    g_file_ptr_ref = &file_ptr_ref;
    g_file_size_ref = &file_size_ref;
    g_vanilla_param_file = old_param_file;
    g_vanilla_param_size = file_size_ref;
    g_expanded_param_file = new_param_file;
    g_expanded_param_size = static_cast<int64_t>(param_file_size);

    file_ptr_ref = new_param_file;
    file_size_ref = static_cast<int64_t>(param_file_size);
    g_param_injection_active = true;

    spdlog::debug("Injection complete: {} total rows", total_rows);
}

// ─── TutorialParam row injection ─────────────────────────────────────
//
// Adds two new rows for the F10 banner: one displays "Map icons: ON", the
// other "Map icons: OFF". Each row is copied from an existing codex row
// (4167000 — guaranteed to exist with menuType=0 / triggerType=0 / repeatType=1
// from ERR's codex data) and then patched so its textId points at our newly
// injected TutorialBody.fmg entries.
//
// Per ERR TutorialParam.xml paramdef (TUTORIAL_PARAM_ST):
//   offset 4  u8 menuType                (0 = upper-left toast widget)
//   offset 5  u8 triggerType
//   offset 6  u8 repeatType
//   offset 16 (0x10) s32 textId          ← FMG id we point at our entries
//   offset 12 u32 unlockEventFlagId      ← cleared, no gate
//   offset 20 (0x14) f32 dispMinTime
//   offset 24 (0x18) f32 dispTime

// (Kept for reference but unused now — see hijack_tutorial_param_textids()
// below for the simpler in-place approach we ship.)
static constexpr int TUTORIAL_TEMPLATE_ROW_ID = 4167000;
static constexpr int TUTORIAL_NEW_ROW_ID_ON        = goblin::TUTORIAL_FMG_ID_ON;
static constexpr int TUTORIAL_NEW_ROW_ID_OFF       = goblin::TUTORIAL_FMG_ID_OFF;
static constexpr int TUTORIAL_NEW_ROW_ID_DUMP_OK   = goblin::TUTORIAL_FMG_ID_DUMP_OK;
static constexpr int TUTORIAL_NEW_ROW_ID_DUMP_FAIL = goblin::TUTORIAL_FMG_ID_DUMP_FAIL;

static ParamResCap *find_param_res_cap_by_name(const wchar_t *target)
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view name = from::params::dlw_c_str(&prc->param_name);
        if (name == target) return prc;
    }
    return nullptr;
}

bool goblin::inject_tutorial_popup_rows()
{
    auto prc = find_param_res_cap_by_name(L"TutorialParam");
    if (!prc)
    {
        spdlog::warn("[TOAST] TutorialParam not found — F10 banner falls back to Summon");
        return false;
    }
    auto *rescap = reinterpret_cast<uint8_t *>(prc->param_header);
    auto *&file_ptr = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size = *reinterpret_cast<int64_t *>(rescap + 0x78);

    auto *old_file = file_ptr;
    auto *old_table = reinterpret_cast<ParamTable *>(old_file);
    uint16_t orig_rows = old_table->num_rows;
    if (orig_rows < 2)
    {
        spdlog::warn("[TOAST] TutorialParam has only {} rows", orig_rows);
        return false;
    }

    // Row data size from TUTORIAL_PARAM_ST paramdef: 1+3 reserve, menuType,
    // triggerType, repeatType, pad1, imageId(u16), pad2(2), unlockEventFlagId
    // (u32), textId(s32), displayMinTime(f32), displayTime(f32), pad3(4) = 32B.
    constexpr int64_t TUTORIAL_ROW_DATA_SIZE = 32;
    int64_t row_data_size = TUTORIAL_ROW_DATA_SIZE;

    // Sanity: the in-memory stride between rows must match the paramdef size.
    int64_t derived_stride = (int64_t)old_table->rows[1].param_offset -
                             (int64_t)old_table->rows[0].param_offset;
    if (derived_stride != row_data_size)
    {
        spdlog::warn("[TOAST] TutorialParam stride {} != paramdef {} — re-laying contiguously",
                     derived_stride, row_data_size);
    }

    // Find template row (row 4167000 — known ERR codex row with menuType=0).
    const uint8_t *template_data = nullptr;
    for (uint16_t i = 0; i < orig_rows; i++)
    {
        if ((int)old_table->rows[i].row_id == TUTORIAL_TEMPLATE_ROW_ID)
        {
            template_data = old_file + old_table->rows[i].param_offset;
            break;
        }
    }
    if (!template_data)
    {
        spdlog::warn("[TOAST] TutorialParam row {} (template) not found — F10 banner unavailable",
                     TUTORIAL_TEMPLATE_ROW_ID);
        return false;
    }

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    constexpr size_t ROW_LOCATOR_SIZE = sizeof(ParamRowInfo);
    constexpr size_t WRAPPER_ROW_LOC_SIZE = sizeof(WrapperRowLocator);

    const char *type_str = reinterpret_cast<const char *>(old_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    uint32_t new_row_count = 4;  // ON, OFF, DUMP_OK, DUMP_FAIL
    uint32_t total_rows = orig_rows + new_row_count;

    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * ROW_LOCATOR_SIZE;
    size_t data_end = data_start + total_rows * (size_t)row_data_size;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    // CRITICAL: align wrapper_row_loc to 16, NOT 4. The lookup-by-id engine
    // (LookupTutorialParam @ eldenring.exe+0xD51BA0) reads this offset from the
    // wrapper header and rounds it UP to 16 via `(x + 0xf) & ~0xf` before using
    // it as the wrapper_row_locator base for its binary search. If our actual
    // array sits at a merely-4-aligned offset, the engine reads 4-12 bytes
    // past it → garbage row ids → out-of-range index → OOB row-data read →
    // crash on save-load (which does an id lookup). WMP got away with 4-align
    // because it's only ever iterated, never id-looked-up.
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * WRAPPER_ROW_LOC_SIZE;
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    auto *allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("[TOAST] HeapAlloc failed ({} bytes) for TutorialParam expansion", total_alloc);
        return false;
    }

    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<ParamTable *>(new_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = (uint32_t)wrapper_row_loc_start;
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = (int32_t)total_rows;

    memcpy(new_file, old_file, HEADER_SIZE);
    new_table->num_rows = (uint16_t)total_rows;
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_file + 0x00) = (uint32_t)type_str_start;
    // Offset 0x04 (ushortDataOffset) left as memcpy'd from original (0): the
    // new ER param format uses the u64 dataOffset @0x30 as canonical source.
    *reinterpret_cast<uint64_t *>(new_file + 0x30) = data_start;

    memcpy(new_file + type_str_start, type_str, type_str_len);

    struct RowSource
    {
        int32_t row_id;
        const uint8_t *data_ptr;
    };
    std::vector<RowSource> all_rows;
    all_rows.reserve(total_rows);
    for (uint16_t i = 0; i < orig_rows; i++)
    {
        auto *data = old_file + old_table->rows[i].param_offset;
        all_rows.push_back({(int32_t)old_table->rows[i].row_id, data});
    }
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_ON,        template_data});
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_OFF,       template_data});
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_DUMP_OK,   template_data});
    all_rows.push_back({TUTORIAL_NEW_ROW_ID_DUMP_FAIL, template_data});

    std::sort(all_rows.begin(), all_rows.end(),
              [](const RowSource &a, const RowSource &b) { return a.row_id < b.row_id; });

    auto *new_locators = reinterpret_cast<ParamRowInfo *>(new_file + row_locators_start);
    auto *new_wrapper_locs = reinterpret_cast<WrapperRowLocator *>(new_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;

    for (size_t i = 0; i < all_rows.size(); i++)
    {
        size_t data_offset = data_start + i * (size_t)row_data_size;
        new_locators[i].row_id = (uint64_t)all_rows[i].row_id;
        new_locators[i].param_offset = data_offset;
        new_locators[i].param_end_offset = file_end_marker;
        memcpy(new_file + data_offset, all_rows[i].data_ptr, (size_t)row_data_size);
        new_wrapper_locs[i].row = all_rows[i].row_id;
        new_wrapper_locs[i].index = (int32_t)i;

        // Patch our new rows: textId -> their own row id (so the FMG-lookup
        // side resolves to the entries we injected separately), clear
        // unlockEventFlagId so no gate prevents display. repeatType stays 1
        // (from template) so each row can show more than once.
        int32_t rid = all_rows[i].row_id;
        if (rid == TUTORIAL_NEW_ROW_ID_ON || rid == TUTORIAL_NEW_ROW_ID_OFF ||
            rid == TUTORIAL_NEW_ROW_ID_DUMP_OK || rid == TUTORIAL_NEW_ROW_ID_DUMP_FAIL)
        {
            auto *p = new_file + data_offset;
            *reinterpret_cast<uint8_t *>(p + 4)  = 0;      // menuType = 0 (toast)
            *reinterpret_cast<uint32_t *>(p + 12) = 0;     // unlockEventFlagId = 0
            *reinterpret_cast<int32_t *>(p + 16)  = rid;   // textId -> our row id
        }
    }

    file_ptr = new_file;
    file_size = (int64_t)param_file_size;

    spdlog::info("[TOAST] TutorialParam expanded: {} -> {} rows (ON={}, OFF={}, DUMP_OK={}, DUMP_FAIL={})",
                 orig_rows, total_rows, TUTORIAL_NEW_ROW_ID_ON, TUTORIAL_NEW_ROW_ID_OFF,
                 TUTORIAL_NEW_ROW_ID_DUMP_OK, TUTORIAL_NEW_ROW_ID_DUMP_FAIL);
    return true;
}


// ─── Runtime param toggle (drives the F10 personal show/hide) ────────

void goblin::set_param_injection_active(bool active)
{
    if (!g_file_ptr_ref)
    {
        spdlog::warn("[TOGGLE] Param swap state not initialized — inject_map_entries() didn't run");
        return;
    }
    if (active == g_param_injection_active)
        return;
    if (active)
    {
        *g_file_ptr_ref = g_expanded_param_file;
        *g_file_size_ref = g_expanded_param_size;
    }
    else
    {
        *g_file_ptr_ref = g_vanilla_param_file;
        *g_file_size_ref = g_vanilla_param_size;
    }
    g_param_injection_active = active;
    spdlog::info("[TOGGLE] WorldMapPointParam -> {}", active ? "EXPANDED" : "VANILLA");
}

bool goblin::is_param_injection_active()
{
    return g_param_injection_active;
}

// Combo is configurable via toggle_gamepad_combo in the ini. Default is
// Y + R3 (right stick click), which is uncommon during normal play. Polled
// on all 4 XInput slots so the order of pad-plug doesn't matter.
static bool gamepad_combo_held()
{
    WORD mask = goblin::config::toggleGamepadMask;
    if (!mask) return false;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
    {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        if ((st.Gamepad.wButtons & mask) == mask)
            return true;
    }
    return false;
}

void goblin::toggle_hotkey_loop()
{
    bool prev_kbd = false, prev_pad = false, prev_f11 = false;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!config::enableToggleHotkey) { prev_kbd = false; prev_pad = false; prev_f11 = false; continue; }

        SHORT state = GetAsyncKeyState(static_cast<int>(config::toggleInjectionKey));
        bool kbd = (state & 0x8000) != 0;
        bool pad = gamepad_combo_held();

        // Rising-edge on either input source independently.
        bool fired = (kbd && !prev_kbd) || (pad && !prev_pad);
        prev_kbd = kbd;
        prev_pad = pad;

        // EXPERIMENT: F11 cycles the toast method (see TOAST_METHOD_NAMES).
        bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !prev_f11)
        {
            int m = (g_toast_method.load() + 1) % TOAST_METHOD_COUNT;
            g_toast_method.store(m);
            spdlog::info("[TOAST] method -> [{}]", TOAST_METHOD_NAMES[m]);
        }
        prev_f11 = f11;

        if (fired)
        {
            // The hotkey is a master-off intent, not a direct param swap. The
            // watcher thread (menu_auto_toggle_loop) is the single owner of the
            // param state; it honours this flag. When disabled the user has
            // explicitly hidden the icons, so they stay hidden even on the map.
            bool disabled = !g_icons_user_disabled.load();
            g_icons_user_disabled.store(disabled);
            spdlog::info("[TOGGLE] icons {} by user (source: {})",
                         disabled ? "HIDDEN" : "SHOWN",
                         (kbd && !pad) ? "keyboard" : (pad && !kbd) ? "gamepad" : "both");
        }
    }
}

// (The old Summon-message path (post_summon) was removed: it depended on five
// hardcoded RVAs (0x763360/0x11A3E0/0x843860/0x844060/0x843910) that a game
// update invalidates, and the codex trampoline below is the toast style we
// actually ship. The F10/F9 banner uses the AOB-resolved trampoline only.)

// ShowTutorialPopup callers — codex/medal upper-left toast.
// Three entries pinned by static analysis (agent run, May 2026):
//   - inner   0x7EF5B0  `void(CSPopupMenu*, int id, bool, bool)` (286-byte fn)
//   - outer   0x7EE630  `void(CSPopupMenu*, int id, bool)` (4 direct call sites)
//   - tramp   0x80DA50  `void(int id)` — resolves singleton internally
// CSPopupMenu singleton ptr lives in .data at `CSFeMan_slot + 0x80`.
// AOB anchor for outer (24 bytes, unique across image):
//   48 8B C4 44 88 40 18 89 50 10 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC
// Patch-resilient anchor: LEA xref in real-.text to string
//   "CS::CSPopupMenu::_CanOpenTutorialParam" in .rdata.
//
// Note: eldenring.exe has TWO `.text` sections (VMProtect adds one). When
// pinning via pefile, scan the original MSVC `.text` at RVA 0x1000..0x29A3000,
// NOT the VMP-added one at 0x4C0E000+ — different content, will miss real fns.
// Resolve the trampoline by AOB (NOT a hardcoded RVA): a game update shifts
// every function's RVA (the May-2026 patch moved this one from 0x80DA50 to
// 0x80D960), so we pin it by a stable surrounding-byte signature that survives
// patches. modutils::scan returns the address of the AOB's first byte = the
// function entry. Resolved once and cached.
static void show_tutorial_popup_trampoline(uintptr_t /*er*/, int tutorial_id)
{
    static void (*fn)(int) = nullptr;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        fn = reinterpret_cast<void (*)(int)>(modutils::scan<void>({
            .aob = "48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9",
        }));
        spdlog::info("[TOAST] trampoline ShowTutorialPopup @ {:p}", (void *)fn);
    }
    if (fn) fn(tutorial_id);
}

// SEH-guarded dispatch of one toast method. POD-only locals (no C++ unwinding).
static void seh_dispatch_toast(int method, uintptr_t er, void * /*mm*/, void *fe,
                               void ** /*csfeman_slot*/, bool icons_on,
                               const wchar_t *text)
{
    (void)method; (void)fe; (void)text;
    int tutorial_id = icons_on ? goblin::TUTORIAL_FMG_ID_ON : goblin::TUTORIAL_FMG_ID_OFF;
    __try
    {
        // Only the AOB-resolved codex trampoline remains (Summon path removed).
        show_tutorial_popup_trampoline(er, tutorial_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Fire a toast using the currently-selected method (cycled by F11). Resolves
// the module base + singleton slots once.
static void show_toggle_banner(bool icons_on)
{
    static bool resolved = false;
    static uintptr_t er = 0;
    static void **menu_man_slot = nullptr;
    static void **fe_man_slot = nullptr;
    if (!resolved)
    {
        resolved = true;
        er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        menu_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24",
            .relative_offsets = {{3, 7}},
        }));
        fe_man_slot = reinterpret_cast<void **>(modutils::scan<void *>({
            .aob = "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 11 8B 80 3C 65 00 00",
            .relative_offsets = {{3, 7}},
        }));
        spdlog::info("[TOAST] resolve er=0x{:X} CSMenuMan_slot={:p} CSFeMan_slot={:p}",
                     er, (void *)menu_man_slot, (void *)fe_man_slot);
    }
    if (!er || !menu_man_slot || !fe_man_slot) return;
    void *mm = *menu_man_slot, *fe = *fe_man_slot;
    if (!mm || !fe) return;

    int method = g_toast_method.load();
    if (method < 0 || method >= TOAST_METHOD_COUNT) method = 0;
    const wchar_t *text = icons_on ? L"Map icons: ON" : L"Map icons: OFF";
    spdlog::info("[TOAST] fire method [{}] (icons {})", TOAST_METHOD_NAMES[method],
                 icons_on ? "ON" : "OFF");
    seh_dispatch_toast(method, er, mm, fe, fe_man_slot, icons_on, text);
}

// SEH-guarded trampoline fire (POD-only locals — no C++ unwinding).
static void seh_fire_trampoline(uintptr_t er, int tutorial_id)
{
    __try { show_tutorial_popup_trampoline(er, tutorial_id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Fire an upper-left codex toast for one of the injected TutorialParam rows
// (a TUTORIAL_FMG_ID_* id). Static text via the same trampoline path as the
// F10 banner — no FMG rewrite. Used by the F9 marker-dump banner.
void goblin::show_codex_toast(int tutorial_id)
{
    static uintptr_t er = 0;
    if (!er) er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;
    seh_fire_trampoline(er, tutorial_id);
}


// WorldMapPointParam state owner. Since the 16-align fix in inject_map_entries
// (see docs/ersc_hosting_and_map_autohide.md), the expanded table is safe during
// ERSC hosting — the old "expand only while the map is open" auto-hide is no
// longer needed and has been removed. The table now stays EXPANDED always; the
// hotkey is a pure personal show/hide toggle.
//
// Desired table state:
//   userDisabled (F10/gamepad master-off) -> VANILLA  (user hid the icons)
//   else                                  -> EXPANDED  (icons everywhere)
//
// (The retired map-state auto-hide read CSMenuMan+0xCD with inverse logic;
// it's fully documented in docs/ersc_hosting_and_map_autohide.md should a
// future patch ever need it back.)
void goblin::menu_auto_toggle_loop()
{
    bool prev_user_disabled = g_icons_user_disabled.load();

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Show the native banner when the user flips the master-off via hotkey.
        // Driven from this thread (the param-state owner) so all game-state
        // mutation happens in one place.
        bool user_disabled_now = g_icons_user_disabled.load();
        if (user_disabled_now != prev_user_disabled)
        {
            show_toggle_banner(!user_disabled_now);
            prev_user_disabled = user_disabled_now;
        }

        bool want_expanded = !user_disabled_now;

        if (want_expanded && !g_param_injection_active)
        {
            set_param_injection_active(true);
            spdlog::info("[TOGGLE] -> EXPANDED (icons on)");
        }
        else if (!want_expanded && g_param_injection_active)
        {
            set_param_injection_active(false);
            spdlog::info("[TOGGLE] -> VANILLA (icons off)");
        }
    }
}
