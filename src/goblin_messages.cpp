#include "goblin_messages.hpp"
#include "goblin_map_data.hpp"
#include "goblin_enemy_names.hpp"
#include "goblin_name_aliases_en.hpp"
#include "goblin_major_regions.hpp"
#include "goblin_config.hpp"
#include "goblin_inject.hpp"
#include "goblin_bench.hpp"
#include "from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"

#include <cstring>
#include <spdlog/spdlog.h>
#include <deque>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace from::CS
{
class MsgRepositoryImp;
}

static from::CS::MsgRepositoryImp *msg_repository = nullptr;
static void *fmg_allocation = nullptr;

// Native message getter (CS::MsgRepositoryImp::GetMessage, FUN_14266d3c0):
//   wchar_t* GetMessage(repo, group, fmgId, msgId);  group=0, fmgId=physical slot.
// Bounds- and null-checks internally, so calling it is crash-safe on every slot
// (incl. ERR's stub DLC slots); under ERR it returns base+DLC-merged content
// because ERR hooks it. Resolved in setup_messages; used by lookup_text on demand
// instead of hand-walking slot internals. RE: docs/re/windows_native_msg_getter_re_findings.md
using GetMessageFn = const wchar_t *(*)(void *repo, uint32_t group, uint32_t fmgId,
                                        uint32_t msgId);
static GetMessageFn g_get_message = nullptr;

// Decode a marker textId into its FMG layer slots (tried dlc02 -> dlc01 -> base)
// and the real FMG id. Inverse of encode_live_item (goblin_inject.cpp) + the
// marker offset bands in setup_messages. Mod-agnostic: GetMessage safely returns
// null for absent/stub slots, so the full layered slot list is always safe.
struct DecodedTextId { const int *slots; int n; int32_t real_id; };
static DecodedTextId decode_textid(int32_t id)
{
    static const int kWeapon[]    = {410, 310, 11};
    static const int kProtector[] = {413, 313, 12};
    static const int kAccessory[] = {416, 316, 13};
    static const int kGem[]       = {422, 322, 35, 42};
    static const int kGoods[]     = {419, 319, 10};
    static const int kEvent[]     = {467, 367, 34};
    static const int kNpc[]       = {428, 328, 18};
    static const int kAction[]    = {32};
    static const int kTutorial[]  = {475, 375, 207};
    static const int kBlood[]     = {461, 361, 2};
    static const int kPlace[]     = {429, 329, 19};
#define MFG_DEC(arr, rid) DecodedTextId{(arr), static_cast<int>(sizeof(arr) / sizeof((arr)[0])), (rid)}
    if (id >= 1600000000 && id < 1700000000) return MFG_DEC(kNpc,       id - 700000000); // NpcName hi-range
    if (id >=  950000000 && id <  960000000) return MFG_DEC(kBlood,     id - 950000000); // BloodMsg
    if (id >=  900000000 && id <  950000000) return MFG_DEC(kTutorial,  id - 900000000); // TutorialTitle
    if (id >=  800000000 && id <  900000000) return MFG_DEC(kAction,    id - 800000000); // ActionButtonText
    if (id >=  700000000 && id <  800000000) return MFG_DEC(kNpc,       id - 700000000); // NpcName
    if (id >=  600000000 && id <  700000000) return MFG_DEC(kEvent,     id - 600000000); // EventTextForMap
    if (id >=  500000000 && id <  600000000) return MFG_DEC(kGoods,     id - 500000000); // GoodsName
    if (id >=  400000000 && id <  500000000) return MFG_DEC(kGem,       id - 400000000); // GemName + ArtsName
    if (id >=  300000000 && id <  400000000) return MFG_DEC(kAccessory, id - 300000000); // AccessoryName
    if (id >=  200000000 && id <  300000000) return MFG_DEC(kProtector, id - 200000000); // ProtectorName
    if (id >=  100000000 && id <  200000000) return MFG_DEC(kWeapon,    id - 100000000); // WeaponName
    if (id >=   50000000 && id <  100000000) return MFG_DEC(kWeapon,    id);             // WeaponName ammo (as-is)
    return MFG_DEC(kPlace, id);                                                          // PlaceName (< 50M)
#undef MFG_DEC
}

// Every PlaceName id that resolves to a real string after the FMG patch.
// Populated in patch_fmg_in_memory; read by sanitize_injected_textids().
static std::unordered_set<int32_t> g_placename_valid_ids;

// Toggle state for PlaceName FMG (slot 19). Only this slot is a pointer
// swap — other slots get surgical in-place additions we don't try to undo.
static uint8_t **g_placename_slot_ptr = nullptr;
static uint8_t *g_vanilla_placename_fmg = nullptr;
static uint8_t *g_expanded_placename_fmg = nullptr;
static bool g_fmg_injection_active = false;

static std::string detect_language()
{
    HMODULE steam = GetModuleHandleA("steam_api64.dll");
    if (!steam) return "english";
    typedef void *(*SteamApps_fn)();
    typedef const char *(*GetLang_fn)(void *);
    auto steamApps = (SteamApps_fn)GetProcAddress(steam, "SteamAPI_SteamApps_v008");
    auto getLang   = (GetLang_fn)GetProcAddress(steam, "SteamAPI_ISteamApps_GetCurrentGameLanguage");
    if (steamApps && getLang)
    {
        void *apps = steamApps();
        if (apps)
        {
            const char *lang = getLang(apps);
            if (lang && lang[0]) return lang;
        }
    }
    return "english";
}

// Map a Steam game-language string (from detect_language) to an index into the
// embedded enemy-name table's ENEMY_NAME_LANGS (msgbnd codes). Falls back to 0
// (engus) for anything unmapped.
static int enemy_name_lang_index(const std::string &steam_lang)
{
    static const std::pair<const char *, const char *> STEAM_TO_MSGBND[] = {
        {"english", "engus"}, {"japanese", "jpnjp"}, {"german", "deude"},
        {"french", "frafr"}, {"italian", "itait"}, {"koreana", "korkr"},
        {"polish", "polpl"}, {"brazilian", "porbr"}, {"portuguese", "porbr"},
        {"russian", "rusru"}, {"spanish", "spaes"}, {"latam", "spaar"},
        {"thai", "thath"}, {"schinese", "zhocn"}, {"tchinese", "zhotw"},
        {"arabic", "araae"},
    };
    const char *code = "engus";
    for (auto &m : STEAM_TO_MSGBND)
        if (steam_lang == m.first) { code = m.second; break; }
    for (int i = 0; i < goblin::generated::ENEMY_NAME_LANG_COUNT; i++)
        if (std::strcmp(code, goblin::generated::ENEMY_NAME_LANGS[i]) == 0)
            return i;
    return 0;
}

// In-memory FMG binary layout (Elden Ring, version 2):
//   0x00: uint32 header    (0x00020000 = version 2 packed)
//   0x04: uint32 fileSize
//   0x08: uint32 unk08     (1)
//   0x0C: uint32 groupCount
//   0x10: uint32 stringCount
//   0x14: uint32 unk14     (0xFF)
//   0x18: uint64 stringOffsetsOffset (relative to FMG start)
//   0x20: 8 bytes zeros
//   0x28: groups[groupCount], each 16 bytes:
//         int32 stringIndex, int32 firstId, int32 lastId, int32 pad(0)
//   stringOffsetsOffset: uint64[stringCount] — each is offset from FMG start to UTF-16LE string
//   after offsets: UTF-16LE null-terminated string data

struct FmgGroup
{
    int32_t string_index;
    int32_t first_id;
    int32_t last_id;
    int32_t pad;
};

struct NewEntry
{
    int32_t id;
    const wchar_t *text;
};

static bool patch_fmg_in_memory(uint8_t *fmg_ptr, uint8_t **slot_ptr,
                                const std::vector<NewEntry> &new_entries_in,
                                bool capture_valid_ids = false)
{
    // De-duplicate injected entries by id (first occurrence wins). Source FMG
    // group tables can cover the same id twice (observed in modded msgbnds),
    // and the two passes below (string-data append, then offset rebuild) MUST
    // agree on exactly which entries carry new strings: a single skipped
    // duplicate desynchronized every later string offset — labels after it
    // showed truncated/foreign text (reported in-game under The Convergence).
    std::vector<NewEntry> new_entries;
    {
        std::unordered_set<int32_t> seen;
        new_entries.reserve(new_entries_in.size());
        for (auto &ne : new_entries_in)
            if (seen.insert(ne.id).second)
                new_entries.push_back(ne);
        if (new_entries.size() != new_entries_in.size())
            spdlog::info("[PATCH] {} duplicate injected id(s) dropped",
                         new_entries_in.size() - new_entries.size());
    }
    uint32_t orig_file_size  = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x04);
    uint32_t orig_group_cnt  = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x0C);
    uint32_t orig_string_cnt = *reinterpret_cast<uint32_t *>(fmg_ptr + 0x10);
    uint64_t raw_str_off     = *reinterpret_cast<uint64_t *>(fmg_ptr + 0x18);

    // Detect fixup: game converts relative offset to absolute pointer at runtime
    uint64_t orig_str_off_rel;
    uint8_t *orig_offsets_ptr;
    if (raw_str_off > 0x1000000)
    {
        orig_offsets_ptr = reinterpret_cast<uint8_t *>(raw_str_off);
        orig_str_off_rel = (uint64_t)(orig_offsets_ptr - fmg_ptr);
    }
    else
    {
        orig_str_off_rel = raw_str_off;
        orig_offsets_ptr = fmg_ptr + orig_str_off_rel;
    }

    spdlog::debug("[PATCH] Original: fileSize={}, groups={}, strings={}, strOffRel=0x{:X} (raw=0x{:X})",
                  orig_file_size, orig_group_cnt, orig_string_cnt, orig_str_off_rel, raw_str_off);

    auto *orig_groups = reinterpret_cast<FmgGroup *>(fmg_ptr + 0x28);
    auto *orig_offsets = reinterpret_cast<uint64_t *>(orig_offsets_ptr);

    struct ExistingEntry
    {
        int32_t id;
        uint64_t str_offset; // offset from FMG start
    };
    std::vector<ExistingEntry> all_entries;
    all_entries.reserve(orig_string_cnt + new_entries.size());

    for (uint32_t g = 0; g < orig_group_cnt; g++)
    {
        int32_t idx = orig_groups[g].string_index;
        int32_t fid = orig_groups[g].first_id;
        int32_t lid = orig_groups[g].last_id;
        for (int32_t id = fid; id <= lid; id++)
        {
            int32_t si = idx + (id - fid);
            if (si >= 0 && si < (int32_t)orig_string_cnt)
                all_entries.push_back({id, orig_offsets[si]});
        }
    }

    spdlog::debug("[PATCH] Parsed {} existing entries", all_entries.size());

    for (size_t i = 0; i < (std::min)(all_entries.size(), (size_t)5); i++)
    {
        auto &e = all_entries[i];
        spdlog::debug("[PATCH] Entry[{}]: id={}, strOff=0x{:X}",
                      i, e.id, e.str_offset);
    }

    // Injected entries OVERRIDE pre-existing ids. Overhaul mods can pre-seed
    // rows at ids our offset encoding also uses (The Convergence ships
    // boss-text PlaceName rows in the 9xxM band, some with EMPTY text —
    // observed shadowing our "Summoning Pools" label at 900301690). Keeping
    // the old row would make the merge below skip ours, so drop it first.
    if (!new_entries.empty())
    {
        std::unordered_set<int32_t> override_ids;
        for (auto &ne : new_entries)
            override_ids.insert(ne.id);
        size_t before = all_entries.size();
        all_entries.erase(std::remove_if(all_entries.begin(), all_entries.end(),
                                         [&](const ExistingEntry &e)
                                         { return override_ids.count(e.id) != 0; }),
                          all_entries.end());
        if (before != all_entries.size())
            spdlog::info("[PATCH] {} existing FMG ids overridden by injected entries",
                         before - all_entries.size());
    }

    uint64_t orig_str_data_start = orig_str_off_rel + orig_string_cnt * 8;

    std::vector<uint8_t> new_str_data;
    size_t orig_str_data_len = orig_file_size - orig_str_data_start;
    new_str_data.resize(orig_str_data_len);
    memcpy(new_str_data.data(), fmg_ptr + orig_str_data_start, orig_str_data_len);

    struct AllEntry
    {
        int32_t id;
        uint64_t str_offset;
    };

    std::vector<AllEntry> merged;
    merged.reserve(all_entries.size() + new_entries.size());

    for (auto &e : all_entries)
        merged.push_back({e.id, e.str_offset});

    struct PendingStr
    {
        size_t merged_idx;
        size_t data_offset; // offset within new_str_data
    };
    std::vector<PendingStr> pending;

    for (auto &ne : new_entries)
    {
        bool exists = false;
        for (auto &m : merged)
        {
            if (m.id == ne.id)
            {
                exists = true;
                break;
            }
        }
        if (exists) continue;

        size_t str_start = new_str_data.size();
        size_t wlen = wcslen(ne.text);
        size_t byte_len = (wlen + 1) * sizeof(wchar_t);
        new_str_data.resize(new_str_data.size() + byte_len);
        memcpy(new_str_data.data() + str_start, ne.text, byte_len);

        pending.push_back({merged.size(), str_start});
        merged.push_back({ne.id, 0}); // offset TBD
    }

    std::sort(merged.begin(), merged.end(),
              [](const AllEntry &a, const AllEntry &b) { return a.id < b.id; });

    // Record every PlaceName id that now resolves to a real string, so
    // sanitize_injected_textids() can strip marker textIds that point at a
    // missing entry (game's GetMessage returns null → wstring(null) → crash).
    // ONLY for the PlaceName (slot 19) patch — this function is also used for
    // the TutorialBody (codex toast) patch, whose id set is unrelated; capturing
    // that would wrongly flag every marker textId as missing and clear them all.
    if (capture_valid_ids)
    {
        g_placename_valid_ids.clear();
        for (auto &m : merged)
            g_placename_valid_ids.insert(m.id);
    }

    uint32_t total_strings = (uint32_t)merged.size();

    spdlog::debug("[PATCH] Total entries after merge: {}", total_strings);

    // One group per entry (sparse IDs)
    uint32_t total_groups = total_strings;

    constexpr size_t HEADER_SIZE = 0x28;
    size_t groups_size = total_groups * sizeof(FmgGroup);
    size_t new_str_off_pos = HEADER_SIZE + groups_size;
    size_t offsets_size = total_strings * sizeof(uint64_t);
    size_t new_str_data_start = new_str_off_pos + offsets_size;
    size_t new_file_size = new_str_data_start + new_str_data.size();

    spdlog::debug("[PATCH] New layout: groups={}, strings={}, fileSize={}",
                  total_groups, total_strings, new_file_size);

    // HeapAlloc (not VirtualAlloc): see goblin_inject.cpp — Seamless Co-op
    // hosting crashes if expanded mod buffers live outside the process heap.
    fmg_allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_file_size);
    if (!fmg_allocation)
    {
        spdlog::error("[PATCH] HeapAlloc failed ({} bytes)", new_file_size);
        return false;
    }

    auto *nfmg = reinterpret_cast<uint8_t *>(fmg_allocation);

    *reinterpret_cast<uint32_t *>(nfmg + 0x00) = 0x00020000;
    *reinterpret_cast<uint32_t *>(nfmg + 0x04) = (uint32_t)new_file_size;
    *reinterpret_cast<uint32_t *>(nfmg + 0x08) = 1;
    *reinterpret_cast<uint32_t *>(nfmg + 0x0C) = total_groups;
    *reinterpret_cast<uint32_t *>(nfmg + 0x10) = total_strings;
    *reinterpret_cast<uint32_t *>(nfmg + 0x14) = 0xFF;
    // Game expects fixed-up absolute pointer, not relative offset
    *reinterpret_cast<uint64_t *>(nfmg + 0x18) = (uint64_t)(nfmg + new_str_off_pos);

    // Rebuild pending map: merged_idx is stale after sort, so re-derive from IDs
    std::unordered_map<int32_t, size_t> new_entry_data_offset;
    {
        size_t data_pos = orig_str_data_len; // where new strings start in new_str_data
        for (auto &ne : new_entries)
        {
            bool exists_in_orig = false;
            for (auto &e : all_entries)
            {
                if (e.id == ne.id) { exists_in_orig = true; break; }
            }
            if (exists_in_orig) continue;

            new_entry_data_offset[ne.id] = new_str_data_start + data_pos;
            size_t wlen = wcslen(ne.text);
            data_pos += (wlen + 1) * sizeof(wchar_t);
        }
    }

    auto *new_groups = reinterpret_cast<FmgGroup *>(nfmg + HEADER_SIZE);
    auto *new_offsets = reinterpret_cast<uint64_t *>(nfmg + new_str_off_pos);

    for (uint32_t i = 0; i < total_strings; i++)
    {
        new_groups[i].string_index = (int32_t)i;
        new_groups[i].first_id = merged[i].id;
        new_groups[i].last_id = merged[i].id;
        new_groups[i].pad = 0;

        if (merged[i].str_offset != 0)
        {
            // Remap old relative offset into new layout
            uint64_t old_off = merged[i].str_offset;
            if (old_off >= orig_str_data_start)
            {
                uint64_t within = old_off - orig_str_data_start;
                new_offsets[i] = new_str_data_start + within;
            }
            else
            {
                new_offsets[i] = 0;
            }
        }
        else
        {
            auto it = new_entry_data_offset.find(merged[i].id);
            if (it != new_entry_data_offset.end())
                new_offsets[i] = it->second;
            else
                new_offsets[i] = 0;
        }
    }

    memcpy(nfmg + new_str_data_start, new_str_data.data(), new_str_data.size());

    spdlog::debug("[PATCH] Swapping FMG pointer: {:p} -> {:p}", (void *)*slot_ptr, fmg_allocation);
    *slot_ptr = nfmg;

    spdlog::debug("[PATCH] PlaceName FMG patched: {} entries", total_strings);
    return true;
}

void goblin::setup_messages()
{
    GOBLIN_BENCH("map.setup_messages.total");
    using namespace goblin::generated;

    auto lang = detect_language();
    spdlog::debug("Detected language: {}", lang);

    std::vector<NewEntry> new_entries;

    auto msg_repository_address = modutils::scan<from::CS::MsgRepositoryImp *>({
        .aob = goblin::sig::MSG_REPOSITORY,
        .relative_offsets = {{3, 7}},
    });
    if (!msg_repository_address) { spdlog::error("MsgRepositoryImp AOB not found"); return; }

    while (!(msg_repository = *msg_repository_address))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    spdlog::debug("MsgRepositoryImp at {:p}", (void *)msg_repository);

    // Resolve the native getter. The AOB anchors the INTERIOR (entry+5) because
    // ERR's MinHook overwrites the prologue, so the entry is (match - 5). Once we
    // have it, lookup_text resolves loot/place/npc names on demand — crash-safe on
    // every slot and mod-agnostic, which is why we no longer hand-walk FMG slots.
    g_get_message = modutils::scan<const wchar_t *(void *, uint32_t, uint32_t, uint32_t)>(
        {.aob = goblin::sig::GETMESSAGE, .offset = -5});
    if (g_get_message)
        spdlog::info("GetMessage resolved at {:p}", (void *)g_get_message);
    else
        spdlog::error("GetMessage AOB not found — loot/DLC names will be unavailable");

    // PlaceName = bnd index 19 in MsgRepositoryImp
    auto *repo = reinterpret_cast<uint8_t *>(msg_repository);
    auto base_array = *reinterpret_cast<uint8_t ***>(repo + 0x08);
    int32_t count2 = *reinterpret_cast<int32_t *>(repo + 0x14);

    if (!base_array || !base_array[0] || 19 >= count2)
    {
        spdlog::error("Cannot navigate to PlaceName FMG slot");
        return;
    }

    auto **sub = reinterpret_cast<uint8_t **>(base_array[0]);

    // Non-ERR builds carry their own localized enemy-name table (the runtime
    // FMG has only generic tutorial entries, no enemy names). Inject the names
    // for the detected language into PlaceName at the same +900M encoding the
    // markers use, so enemy-drop labels read properly instead of falling back to
    // the generic vocabulary words. The ERR build ships an empty table and uses
    // its runtime names, so this loop is a no-op there.
    if (generated::ENEMY_NAME_COUNT > 0)
    {
        int li = enemy_name_lang_index(lang);
        int added = 0;
        for (size_t i = 0; i < generated::ENEMY_NAME_COUNT; i++)
        {
            const auto &en = generated::ENEMY_NAMES[i];
            const wchar_t *nm = en.names[li] ? en.names[li] : en.names[0];
            if (nm && nm[0])
            {
                new_entries.push_back({en.id + 900000000, nm});
                added++;
            }
        }
        spdlog::info("Injected {} enemy names into PlaceName (lang index {})", added, li);
    }

    auto *fmg_ptr = sub[19];

    if (!fmg_ptr)
    {
        spdlog::error("PlaceName FMG (bnd=19) is null");
        return;
    }

    spdlog::debug("PlaceName FMG at {:p}", (void *)fmg_ptr);

    // Cluster labels: one static "<count>" string per cluster (clusters are
    // icon-only by default; the F11 debug toggle points a cluster's textId1 here).
    // reserve() so the c_str() pointers stay valid until the patch call below.
    std::vector<std::wstring> cluster_label_storage;
    cluster_label_storage.reserve(goblin::cluster_label_census().size());
    for (auto &cc : goblin::cluster_label_census())
    {
        // cc.second = "<Region> (<count>)" (ASCII) → widen to wstring.
        cluster_label_storage.emplace_back(cc.second.begin(), cc.second.end());
        new_entries.push_back({cc.first, cluster_label_storage.back().c_str()});
    }

    // Major-region last-resort labels: inject each anchor's name at its fresh
    // label_id (the source WorldMapPlaceNameParam textId is in the region-banner FMG
    // bank, not usable as a map-point label). Storage must outlive the patch call.
    std::vector<std::wstring> major_region_storage;
    major_region_storage.reserve(goblin::generated::MAJOR_REGION_ANCHOR_COUNT);
    for (size_t i = 0; i < goblin::generated::MAJOR_REGION_ANCHOR_COUNT; i++)
    {
        const auto &g = goblin::generated::MAJOR_REGION_ANCHORS[i];
        const char *n = g.name;
        major_region_storage.emplace_back(n, n + std::char_traits<char>::length(n));  // ASCII → wide
        new_entries.push_back({g.label_id, major_region_storage.back().c_str()});
    }

    if (patch_fmg_in_memory(fmg_ptr, &sub[19], new_entries, /*capture_valid_ids=*/true))
    {
        spdlog::info("PlaceName FMG patched ({} entries)", new_entries.size());
        // Capture toggle state. fmg_ptr was the original buffer; sub[19] now
        // points at our expanded buffer (set inside patch_fmg_in_memory).
        g_placename_slot_ptr = &sub[19];
        g_vanilla_placename_fmg = fmg_ptr;
        g_expanded_placename_fmg = sub[19];
        g_fmg_injection_active = true;
    }
    else
        spdlog::error("PlaceName FMG patching failed");

    // Inject NEW TutorialBody entries (slot 208 = 0xD0) for the codex toasts.
    // CSPopupMenu::ShowTutorialPopup (trampoline, AOB-resolved at runtime — its
    // RVA shifts on game updates) looks up TutorialParam[id].textId then
    // TutorialBody.fmg[textId]. Matching TutorialParam rows are injected by
    // goblin::inject_tutorial_popup_rows. Using fresh ids leaves all vanilla/ERR
    // codex text untouched. All four entries (ON/OFF/DUMP_OK/DUMP_FAIL) are
    // STATIC strings written below — there is no runtime text rewriting.
    if (count2 > 208 && sub[208])
    {
        std::vector<NewEntry> tb_entries = {
            {goblin::TUTORIAL_FMG_ID_ON,        L"Map icons: ON"},
            {goblin::TUTORIAL_FMG_ID_OFF,       L"Map icons: OFF"},
            {goblin::TUTORIAL_FMG_ID_DUMP_OK,   L"Markers dumped"},
            {goblin::TUTORIAL_FMG_ID_DUMP_FAIL, L"Marker dump failed — press again"},
            {goblin::TUTORIAL_FMG_ID_COVERAGE_GAP,
             L"Map for Goblins: collected an unmapped item (coverage gap — see log)"},
        };
        // Per-category coverage-gap banners: "Unmapped <category> collected".
        // gap_texts owns the wide strings (NewEntry.text is borrowed) until the
        // patch call below returns.
        std::vector<std::wstring> gap_texts;
        gap_texts.reserve(goblin::GAP_CAT_COUNT);
        for (int c = 0; c < goblin::GAP_CAT_COUNT; c++)
        {
            gap_texts.push_back(std::wstring(L"Map for Goblins: unmapped ") +
                                goblin::GAP_CAT_NAMES[c] + L" collected (coverage gap)");
            tb_entries.push_back({goblin::gap_cat_toast_id(c), gap_texts.back().c_str()});
        }

        if (patch_fmg_in_memory(sub[208], &sub[208], tb_entries))
            spdlog::info("[TOAST] TutorialBody.fmg expanded (ON={}, OFF={}, DUMP_OK={}, DUMP_FAIL={})",
                         goblin::TUTORIAL_FMG_ID_ON, goblin::TUTORIAL_FMG_ID_OFF,
                         goblin::TUTORIAL_FMG_ID_DUMP_OK, goblin::TUTORIAL_FMG_ID_DUMP_FAIL);
        else
            spdlog::warn("[TOAST] TutorialBody.fmg patch failed — codex banners unavailable");
    }
    else
        spdlog::warn("[TOAST] TutorialBody (slot 208) unavailable — codex banners unavailable");

    // Final safety pass: strip any marker textId that didn't end up with a real
    // string in the expanded PlaceName FMG. The game's GetMessage returns null
    // for a missing id and some callers build a std::wstring from it → null
    // deref in game code on load transitions (grace teleport / character swap).
    // Root cause was e.g. a double-offset enemy-name id (npc id already +900M
    // tutorial-encoded, then +700M again → 1.6B, in no FMG). Clearing it to -1
    // makes that text line simply absent instead of crashing.
    goblin::sanitize_injected_textids();
}

void goblin::sanitize_injected_textids()
{
    if (!g_expanded_placename_fmg && !g_get_message)
    {
        spdlog::warn("[SANITIZE] no PlaceName FMG and no GetMessage — skipping");
        return;
    }
    auto valid = [](int32_t id) {
        // Logic sentinels the DLL/engine special-case (camp/boss markers) — leave alone.
        if (id == 5000 || id == 5100 || id == 5300 || id == 8800) return true;
        // Valid iff a real string resolves — expanded FMG (mod content) or the
        // native getter (game/DLC names). Mirrors exactly what markers render.
        const wchar_t *s = goblin::lookup_text(id);
        return s != nullptr && s[0] != L'\0';
    };
    const auto &rows = goblin::injected_row_ptrs();
    int cleared = 0, scanned = 0;
    for (uint8_t *p : rows)
    {
        auto *row = reinterpret_cast<from::paramdef::WORLD_MAP_POINT_PARAM_ST *>(p);
        int32_t *tids[8]  = {&row->textId1, &row->textId2, &row->textId3, &row->textId4,
                             &row->textId5, &row->textId6, &row->textId7, &row->textId8};
        unsigned int *fl[8] = {&row->textDisableFlagId1, &row->textDisableFlagId2,
                               &row->textDisableFlagId3, &row->textDisableFlagId4,
                               &row->textDisableFlagId5, &row->textDisableFlagId6,
                               &row->textDisableFlagId7, &row->textDisableFlagId8};
        ++scanned;
        for (int i = 0; i < 8; ++i)
        {
            int32_t id = *tids[i];
            if (id >= 0 && !valid(id))
            {
                *tids[i] = -1;
                *fl[i] = 0;
                ++cleared;
            }
        }
    }
    spdlog::info("[SANITIZE] Scanned {} injected rows, cleared {} dangling textId(s) "
                 "(missing from PlaceName FMG → would null-deref on load)", scanned, cleared);
}

void goblin::set_fmg_injection_active(bool active)
{
    if (!g_placename_slot_ptr)
    {
        spdlog::warn("[TOGGLE] FMG swap state not initialized — setup_messages didn't run");
        return;
    }
    if (active == g_fmg_injection_active)
        return;
    *g_placename_slot_ptr = active ? g_expanded_placename_fmg : g_vanilla_placename_fmg;
    g_fmg_injection_active = active;
    spdlog::info("[TOGGLE] PlaceName FMG -> {}", active ? "EXPANDED" : "VANILLA");
}

bool goblin::is_fmg_injection_active()
{
    return g_fmg_injection_active;
}

// Mod-injected content only (real PlaceName locations + injected cluster /
// major-region / enemy-name labels) — these live in our expanded PlaceName FMG.
static const wchar_t *lookup_expanded(int32_t id)
{
    uint8_t *fmg = g_expanded_placename_fmg;
    if (!fmg || id <= 0)
        return nullptr;
    // Same FMG-v2 layout + relative/absolute offset fixup as patch_fmg_in_memory.
    uint32_t group_cnt  = *reinterpret_cast<uint32_t *>(fmg + 0x0C);
    uint32_t string_cnt = *reinterpret_cast<uint32_t *>(fmg + 0x10);
    uint64_t raw        = *reinterpret_cast<uint64_t *>(fmg + 0x18);
    uint64_t off_rel = (raw > 0x1000000)
                           ? static_cast<uint64_t>(reinterpret_cast<uint8_t *>(raw) - fmg)
                           : raw;
    auto *groups  = reinterpret_cast<FmgGroup *>(fmg + 0x28);
    auto *offsets = reinterpret_cast<uint64_t *>(fmg + off_rel);
    for (uint32_t g = 0; g < group_cnt; ++g)
    {
        if (id >= groups[g].first_id && id <= groups[g].last_id)
        {
            int32_t si = groups[g].string_index + (id - groups[g].first_id);
            if (si < 0 || si >= static_cast<int32_t>(string_cnt))
                return nullptr;
            uint64_t so = offsets[si];
            const wchar_t *s = (so > 0x1000000)
                                   ? reinterpret_cast<const wchar_t *>(so)
                                   : reinterpret_cast<const wchar_t *>(fmg + so);
            return (s && *s) ? s : nullptr;
        }
    }
    return nullptr;
}

const wchar_t *goblin::lookup_text(int32_t id)
{
    if (id <= 0)
        return nullptr;
    // 1) Mod-injected content (real PlaceName locations + cluster/region/enemy
    //    labels) lives only in our expanded PlaceName FMG.
    if (const wchar_t *s = lookup_expanded(id))
        return s;
    // 2) Game FMG strings (loot/place/npc/...) via the native getter — DLC-merged
    //    under ERR, and crash-safe (GetMessage bounds- and null-checks every slot
    //    internally). Cached per-thread; misses are cached too.
    if (!g_get_message || !msg_repository)
        return nullptr;
    static thread_local std::unordered_map<int32_t, const wchar_t *> cache;
    auto it = cache.find(id);
    if (it != cache.end())
        return it->second;
    const wchar_t *res = nullptr;
    DecodedTextId d = decode_textid(id);
    for (int i = 0; i < d.n; i++)
    {
        const wchar_t *s = g_get_message(msg_repository, 0,
                                         static_cast<uint32_t>(d.slots[i]),
                                         static_cast<uint32_t>(d.real_id));
        if (s && s[0])
        {
            res = s;
            break;
        }
    }
    cache[id] = res;
    return res;
}

std::string goblin::lookup_text_utf8(int32_t id)
{
    const wchar_t *w = lookup_text(id);
    if (!w || !w[0])
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string goblin::lookup_name_alias_en_utf8(int32_t id)
{
    // Binary-search the bundled English alias table (sorted ascending by id).
    const auto *begin = goblin::generated::NAME_ALIASES_EN;
    const auto *end = begin + goblin::generated::NAME_ALIAS_EN_COUNT;
    const auto *it = std::lower_bound(begin, end, id,
        [](const goblin::generated::NameAliasEn &e, int32_t key) { return e.id < key; });
    if (it == end || it->id != id || !it->name || !it->name[0])
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, it->name, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, it->name, -1, &s[0], n, nullptr, nullptr);
    return s;
}
