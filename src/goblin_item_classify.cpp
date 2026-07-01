#include "goblin_inject.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "from/params.hpp"
#include "goblin_map_data.hpp"
#include "goblin_category_exceptions.hpp"

#include <map>
#include <optional>
#include <string>

//
// Live item/loot classification (taxonomy-based) — split out of goblin_inject.cpp
// 2026-07-01 (docs/plans/goblin_inject_refactor_plan.md PR 2). Pure relocation, no
// logic changes. Distinct from goblin_loot_resolve.cpp (PR 0, ItemLotParam-ROW-based
// live loot resolve) — this file classifies by ER's own (goodsType, sortGroupId)
// taxonomy, no per-item table. Non-contiguous in the original file (taxonomy
// helpers were forward-declared early, defined ~1450 lines later); both halves
// moved here since they only depend on each other (goods_type_live/goods_sort_group
// forward-declared in the first half, defined in the second). No accessor header
// needed — audit found zero coupling to anything outside these two spans.
// 9 public functions declared in goblin_inject.hpp (facade kept): item_marker_category,
// item_real_icon_id, aeg_pickup_lot, aeg_is_gather, npc_loot_lot, npc_item_lot_enemy,
// npc_team_and_name, goods_is_map, classify_item_live.
//

// ── Phase-3 item classification (ER's OWN taxonomy, no per-item drift) ─────────────────────────
// EquipParamGoods.goodsType@+0x3e + sortGroupId@+0x72 classify every goods (incl. any mod/DLC item)
// live, with ZERO per-item table. Defined later (~goods_sort_group); forward-declared here so the
// public classifier below can use them.
static int goods_type_live(int32_t goods_id);
static int goods_sort_group(int32_t goods_id);

// Curated id → Category exception (binary search the generated table). These are the splits/grab-bags
// the mod assigns by a deliberate id-list rule that (goodsType, sortGroupId) cannot express (runes /
// smithing Low-normal-Rare, Gloveworts vs Great, the gType1-sg50 grab-bag, Quest-Progression +
// Prayerbook id-lists, Reforged families). Returns the Category int, or -1 if the id has no exception.
static int lookup_category_exception(int32_t goods_id)
{
    const auto *begin = goblin::generated::CATEGORY_EXCEPTIONS;
    const auto *end   = begin + goblin::generated::CATEGORY_EXCEPTION_COUNT;
    const auto *it = std::lower_bound(begin, end, goods_id,
        [](const goblin::generated::CategoryException &a, int32_t k) { return a.id < k; });
    return (it != end && it->id == goods_id) ? (int)it->category : -1;
}

// Confident category from ER's live taxonomy for a goods id: the explicit (goodsType, sortGroupId)
// cells + the broad goodsType families. Returns -1 when only the DEFAULT/catch-all bucket applies
// (gType1 key-item tail → QuestProgression, or the LootCraftingMaterials catch-all) — those are the
// "uncertain" surface, left to classify_item_live so the [ITEMCLASS] census still tracks them.
// Validated to reproduce the old ITEM_ICONS category column exactly (tools/_validate_taxonomy_map.py).
static int category_from_taxonomy(int32_t goods_id)
{
    using C = goblin::generated::Category;
    // ERR renumbers spirit-ash goods into id 300000-399999 (gType0 sortGroupId 15) — the same range
    // generate_loot_massedit classifies on. Key on the RANGE, not the sg15 cell: that cell is
    // contaminated by a few non-spirit items (Codex of the All-Knowing, Spectral Steed Whistle,
    // Memory of Grace) which would otherwise be mislabelled as Spirits. Vanilla spirits stay gType 7/8.
    if (goods_id >= 300000 && goods_id <= 399999) return (int)C::EquipSpirits;
    const int gt = goods_type_live(goods_id);
    const int sg = goods_sort_group(goods_id);
    switch (gt)
    {
        case 0:  // normal goods — split by sortGroupId
            switch (sg)
            {
                case 20: case 61: return (int)C::LootConsumables;
                case 50:          return (int)C::LootThrowables;
                case 70:          return (int)C::LootGreases;
                case 60:          return (int)C::LootReusables;
                case 80:          return (int)C::LootUtilities;        // Pates = exception
                case 10:          return (int)C::LootStatBoosts;       // Rune Arc 150 = exception
                case 100: case 101: case 102: return (int)C::LootGoldenRunes;  // Low = exception;
                                                                               // 102 = ERR high-NG variants
            }
            break;
        case 1:  // key/important item — split by sortGroupId; tail handled by the gType1 default below
            switch (sg)
            {
                case 80: case 90:   return (int)C::LootBellBearings;
                case 200: case 205: return (int)C::KeyCookbooks;
                case 100:           return (int)C::MagicPrayerbooks;   // 8867 = exception
            }
            break;
        case 2:  return (int)C::LootCraftingMaterials;   // gather materials
        case 5:  case 17: return (int)C::MagicSorceries;
        case 16: case 18: return (int)C::MagicIncantations;
        case 7:  case 8:  return (int)C::EquipSpirits;
        case 10: if (sg == 20) return (int)C::KeyCrystalTears; break;
        case 11: if (sg == 30) return (int)C::KeyPotsNPerfumes; break;
        case 14: // upgrade material
            switch (sg)
            {
                case 40: case 41:           return (int)C::LootGloveworts;     // Great = exception
                case 19: case 20: case 30:  return (int)C::LootSmithingStones; // Low/Rare = exception
            }
            break;
    }
    return -1;  // only the default/catch-all applies
}

// Primary item → Category classifier (Phase-3). Deterministic, drift-free, no per-item table:
//   1. non-goods → ItemLotParam category by key range (weapon/armour/talisman/AoW + ammo id≥50M)
//   2. curated id exception (the splits ER's taxonomy can't express)
//   3. ER's live taxonomy (goodsType, sortGroupId) — the bulk
//   4. returns -1 for the default/catch-all tail → classify_item_live owns it (and flags it live).
int goblin::item_marker_category(int32_t key)
{
    if (key <= 0) return -1;
    if (key < 500000000)  // non-goods (offset-encoded equip category)
    {
        if (key >= 400000000) return (int)goblin::generated::Category::EquipAshesOfWar;   // gem
        if (key >= 300000000) return (int)goblin::generated::Category::EquipTalismans;    // accessory
        if (key >= 200000000) return (int)goblin::generated::Category::EquipArmour;       // protector
        if (key >= 100000000)  // weapon / ammo (WeaponName.fmg, +100M)
            return (key - 100000000 >= 50000000) ? (int)goblin::generated::Category::LootAmmo
                                                 : (int)goblin::generated::Category::EquipArmaments;
        return -1;  // unknown encoding
    }
    const int32_t gid = key - 500000000;
    const int ex = lookup_category_exception(gid);
    if (ex >= 0) return ex;
    return category_from_taxonomy(gid);  // -1 → default/catch-all, handled by classify_item_live
}

// One AssetEnvironmentGeometryParam row (320 bytes; pickUpItemLotParamId @ +0xb8,
// s32 — offset confirmed vs the paramdef DetectedSize=320). Read by raw offset
// like RawItemLotRow.
struct RawAegRow { uint8_t b[320]; };

// Resolve a placed AEG asset's collectible item-lot LIVE: the disk parser gives the
// aegRow (AEG{A}_{B} -> A*1000+B); this returns AssetEnvironmentGeometryParam
// [aegRow].pickUpItemLotParamId (an ItemLotParam_map id), or 0 if the asset isn't a
// pickup / the row is absent. Item identity then comes from resolve_loot_item_textid.
// No bake, no manual model->item table — pure live param chain (any mod).
uint32_t goblin::aeg_pickup_lot(uint32_t aegRow)
{
    if (aegRow == 0) return 0;
    static std::optional<from::params::ParamTableSequence<RawAegRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    // Worker-thread safe (same rationale as resolve_loot_item_textid).
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawAegRow>(L"AssetEnvironmentGeometryParam"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok) return 0;
    RawAegRow *row = s_seq->try_get(aegRow);
    if (!row) return 0;
    // pickUpItemLotParamId offset, READ LIVE from the game's own `mov eax,[rax+0xb8]` (the asset-
    // pickup lot resolve; pinned by the embedded find-what-accesses). Pinned 0xb8 = logged fallback.
    static const ptrdiff_t s_off = [] {
        auto r = modutils::resolve_field_offset(
            {.aob = goblin::sig::AEG_PICKUP_LOT_ACCESS, .disp_pos = 2, .disp_size = 4});
        if (r) { spdlog::info("[FIELDOFF] AssetEnvironmentGeometryParam.pickUpItemLotParamId = "
                              "+0x{:x} (live from exe)", *r); return *r; }
        spdlog::warn("[FIELDOFF] AEG pickUpItemLotParamId AOB unresolved — falling back to pinned +0xb8");
        return static_cast<ptrdiff_t>(0xb8);
    }();
    uint32_t lot;
    std::memcpy(&lot, row->b + s_off, 4);
    // pickUpItemLotParamId defaults to -1 (0xFFFFFFFF) for non-pickup assets (the
    // param has a row for EVERY asset model); 0 also = none. Either → not a pickup.
    if (lot == 0 || lot == 0xffffffffu) return 0;
    return lot;
}

// isEnableRepick = AssetGeometryParam byte 0x3c **bit 5** (mask 0x20). EMPIRICALLY pinned by
// dumping raw param rows: gather (en=1) byte0x3c=0x60, pot (en=0/break=1) byte0x3c=0x40 → bit 5
// (0x20) = isEnableRepick, bit 6 (0x40) = isBreakOnPickUp. (dummy8 Reserve_2 takes no bit slot, so
// it's bit 5 not 6.) Reading 0x40 by mistake matched EVERY breakable pickup (~18k pots/jars) → the
// Crafting-Materials 16k leak. isEnableRepick ⇔ isHiddenOnRepick for every pickup asset (checked
// offline), so this one bit is the bake's gather filter. Same RawAegRow param table as aeg_pickup_lot.
bool goblin::aeg_is_gather(uint32_t aegRow)
{
    if (aegRow == 0) return false;
    static std::optional<from::params::ParamTableSequence<RawAegRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawAegRow>(L"AssetEnvironmentGeometryParam"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok) return false;
    RawAegRow *row = s_seq->try_get(aegRow);
    if (!row) return false;
    // isEnableRepick: BOTH the byte offset (0x3c) AND the bit (5) are READ LIVE from the game's own
    // `movzx eax,[rax+0x3c]; shr eax,5; and eax,1` repick-eligibility read (pinned by the embedded
    // find-what-accesses). Zero magic numbers — and it live-re-confirms the 16k-leak fix is bit 5,
    // not the Paramdex's bit 6. Pinned 0x3c/bit5 = logged fallback if the AOB breaks after a patch.
    static const std::pair<ptrdiff_t, int> s_repick = [] {
        auto off = modutils::resolve_field_offset(
            {.aob = goblin::sig::AEG_REPICK_BIT_ACCESS, .disp_pos = 3, .disp_size = 1});
        auto bit = modutils::resolve_field_offset(
            {.aob = goblin::sig::AEG_REPICK_BIT_ACCESS, .disp_pos = 6, .disp_size = 1});
        if (off && bit)
        {
            spdlog::info("[FIELDOFF] AssetEnvironmentGeometryParam.isEnableRepick = +0x{:x} bit{} "
                         "(live from exe)", *off, (int)*bit);
            return std::make_pair(*off, (int)*bit);
        }
        spdlog::warn("[FIELDOFF] AEG isEnableRepick AOB unresolved — falling back to pinned +0x3c bit5");
        return std::make_pair(static_cast<ptrdiff_t>(0x3c), 5);
    }();
    return ((row->b[s_repick.first] >> s_repick.second) & 1) != 0;
}

// NpcParam row (736 bytes = NPC_PARAM_ST DetectedSize; itemLotId_enemy s32 @ +0x30,
// itemLotId_map s32 @ +0x34 — offsets pinned vs the applied paramdef, see memory
// msbe-enemy-loot-offsets / docs/re/windows_enemy_loot_nobake_analysis.md).
struct RawNpcRow { uint8_t b[736]; };

// Resolve a placed enemy's drop lot LIVE: the disk parser gives the npcParamId (from
// MSB Parts.Enemies); this returns its NpcParam item lot, preferring itemLotId_map
// (an ItemLotParam_map row, *lotType=1) over itemLotId_enemy (ItemLotParam_enemy,
// *lotType=2) — the same precedence as the offline pipeline. 0 if the NPC drops
// nothing / the row is absent. Item identity then comes from resolve_loot_item_textid.
uint32_t goblin::npc_loot_lot(uint32_t npcParamId, uint8_t *lotTypeOut)
{
    if (lotTypeOut) *lotTypeOut = 0;
    if (npcParamId == 0) return 0;
    static std::optional<from::params::ParamTableSequence<RawNpcRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    // Worker-thread safe (same rationale as resolve_loot_item_textid / aeg_pickup_lot).
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawNpcRow>(L"NpcParam"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok) return 0;
    RawNpcRow *row = s_seq->try_get((uint64_t)npcParamId);
    if (!row) return 0;
    int32_t lotMap, lotEnemy;
    std::memcpy(&lotMap, row->b + 0x34, 4);
    std::memcpy(&lotEnemy, row->b + 0x30, 4);
    if (lotMap > 0) { if (lotTypeOut) *lotTypeOut = 1; return (uint32_t)lotMap; }
    if (lotEnemy > 0) { if (lotTypeOut) *lotTypeOut = 2; return (uint32_t)lotEnemy; }
    return 0;
}

int32_t goblin::npc_item_lot_enemy(uint32_t npcParamId)
{
    if (npcParamId == 0) return 0;
    static std::optional<from::params::ParamTableSequence<RawNpcRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawNpcRow>(L"NpcParam"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok) return 0;
    RawNpcRow *row = s_seq->try_get((uint64_t)npcParamId);
    if (!row) return 0;
    int32_t lotEnemy;
    std::memcpy(&lotEnemy, row->b + 0x30, 4);
    return lotEnemy > 0 ? lotEnemy : 0;
}

bool goblin::npc_team_and_name(uint32_t npcParamId, uint8_t *teamOut, int32_t *nameOut)
{
    if (npcParamId == 0) return false;
    static std::optional<from::params::ParamTableSequence<RawNpcRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawNpcRow>(L"NpcParam"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok) return false;
    RawNpcRow *row = s_seq->try_get((uint64_t)npcParamId);
    if (!row) return false;
    if (teamOut) *teamOut = row->b[0x133];          // teamType u8
    if (nameOut) std::memcpy(nameOut, row->b + 0x0c, 4);  // nameId s32
    return true;
}

// EquipParamGoods row (176 bytes; goodsType u8 @ +0x3e — offset confirmed vs raw rows).
struct RawGoodsRow { uint8_t b[176]; };

// goodsType field offset, READ LIVE from the game's own access instruction (the "industrial
// offset-free" mechanism): modutils::resolve_field_offset AOB-scans `cmp byte [rcx+0x3e],0Dh` and
// extracts the disp8 — so the offset is authoritative + self-correcting across patches, no hardcoded
// constant. Resolved once. The pinned 0x3e is only a LOGGED safety net for the case where a patch
// breaks the AOB (the [SIG] health check flags it too). See docs/re/offset_source_of_truth_audit.md.
static ptrdiff_t goods_type_offset()
{
    static const ptrdiff_t off = [] {
        auto r = modutils::resolve_field_offset(
            {.aob = goblin::sig::GOODS_TYPE_ACCESS, .disp_pos = 7, .disp_size = 1});
        if (r)
        {
            spdlog::info("[FIELDOFF] EquipParamGoods.goodsType = +0x{:x} (live from exe)", *r);
            return *r;
        }
        spdlog::warn("[FIELDOFF] goodsType access AOB unresolved — falling back to pinned +0x3e");
        return static_cast<ptrdiff_t>(0x3e);
    }();
    return off;
}

// goodsType of an EquipParamGoods row (ER EQUIP_GOODS_TYPE), or -1 if absent.
static int goods_type_live(int32_t goods_id)
{
    static std::optional<from::params::ParamTableSequence<RawGoodsRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawGoodsRow>(L"EquipParamGoods"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok || goods_id <= 0) return -1;
    RawGoodsRow *r = s_seq->try_get((uint64_t)goods_id);
    return r ? (int)r->b[goods_type_offset()] : -1;
}

// sortGroupId (u8 @ +0x72) of an EquipParamGoods row, or -1 if absent. This is ER's OWN
// fine-grained item taxonomy — the field that groups the inventory menu — so it classifies
// every item (incl. DLC / mod-added) with ZERO per-item table and zero drift. Offset+type pinned
// via the paramdef field-layout walk (reproduces the known goodsType@0x3e); it is a 1-byte field
// (NOT s16 — reading 2 bytes folds in the 0x73 bitfield). Known rune groups: 100 = base
// Golden/Numen's/Hero's/Lord's Runes, 101 = DLC Broken/Shadow Realm Runes, 102 = ERR high-NG
// variants (Golden One's/Ancient's/…); map fragments: 190 base, 191 DLC.
// sortGroupId field offset, READ LIVE from the game's own `movzx ebx,[rax+0x72]` (the inventory
// item-grouping read; pinned by the embedded find-what-accesses). Same self-correcting contract as
// goods_type_offset(): the pinned 0x72 is only a logged fallback if the AOB breaks after a patch.
static ptrdiff_t goods_sort_group_offset()
{
    static const ptrdiff_t off = [] {
        auto r = modutils::resolve_field_offset(
            {.aob = goblin::sig::GOODS_SORT_GROUP_ACCESS, .disp_pos = 3, .disp_size = 1});
        if (r)
        {
            spdlog::info("[FIELDOFF] EquipParamGoods.sortGroupId = +0x{:x} (live from exe)", *r);
            return *r;
        }
        spdlog::warn("[FIELDOFF] sortGroupId access AOB unresolved — falling back to pinned +0x72");
        return static_cast<ptrdiff_t>(0x72);
    }();
    return off;
}

static int goods_sort_group(int32_t goods_id)
{
    static std::optional<from::params::ParamTableSequence<RawGoodsRow>> s_seq;
    static std::once_flag s_once;
    static bool s_ok = false;
    std::call_once(s_once, [] {
        try { s_seq.emplace(from::params::get_param<RawGoodsRow>(L"EquipParamGoods"));
              s_ok = true; } catch (...) { s_ok = false; }
    });
    if (!s_ok || goods_id <= 0) return -1;
    RawGoodsRow *r = s_seq->try_get((uint64_t)goods_id);
    return r ? (int)r->b[goods_sort_group_offset()] : -1;
}

// Real inventory iconId (the MENU_ItemIcon_<id> atlas index) for an offset-encoded marker/item key,
// read LIVE from the owning EquipParam at the iconId offset cross-verified in verify_equip_iconids()
// (self_calibrate_iconid() finds the same offsets with zero hardcoding — survives patches/mod swaps):
//   goods+500M     → EquipParamGoods.iconId      @0x30
//   weapon/ammo+100M → EquipParamWeapon.iconId    @0xBE
//   protector+200M → EquipParamProtector.iconIdM  @0xA6
//   accessory+300M → EquipParamAccessory.iconId   @0x26
//   gem+400M       → EquipParamGem.iconId         @0x04
// Returns -1 if the key isn't an item or the row/param is absent. Drives the category GPU icon
// (a representative item per category → its real game icon, harvested from the 00_Solo atlas).
struct RawEquipRow { uint8_t b[0x200]; };  // ≥ the largest equip row stride (Protector 0x1a0)

static int read_equip_icon(const wchar_t *param, int32_t real_id, ptrdiff_t off)
{
    static std::mutex s_mx;
    static std::map<std::wstring, std::optional<from::params::ParamTableSequence<RawEquipRow>>> s_seqs;
    if (real_id <= 0) return -1;
    std::lock_guard<std::mutex> lk(s_mx);
    auto it = s_seqs.find(param);
    if (it == s_seqs.end())
    {
        std::optional<from::params::ParamTableSequence<RawEquipRow>> seq;
        try { seq.emplace(from::params::get_param<RawEquipRow>(param)); } catch (...) {}
        it = s_seqs.emplace(param, std::move(seq)).first;
    }
    if (!it->second) return -1;
    RawEquipRow *r = it->second->try_get((uint64_t)real_id);
    return r ? (int)*reinterpret_cast<uint16_t *>(r->b + off) : -1;
}

int goblin::item_real_icon_id(int32_t key)
{
    if (key <= 0) return -1;
    if (key >= 500000000) return read_equip_icon(L"EquipParamGoods",     key - 500000000, 0x30);
    if (key >= 400000000) return read_equip_icon(L"EquipParamGem",       key - 400000000, 0x04);
    if (key >= 300000000) return read_equip_icon(L"EquipParamAccessory", key - 300000000, 0x26);
    if (key >= 200000000) return read_equip_icon(L"EquipParamProtector", key - 200000000, 0xA6);
    if (key >= 100000000) return read_equip_icon(L"EquipParamWeapon",    key - 100000000, 0xBE); // weapon/ammo
    return -1;
}

// True iff an EquipParamGoods row is a region Map fragment — sortGroupId ∈ {190 (base), 191 (DLC)}.
// Used by the no-bake World - Maps pass to route map-good treasure pickups to the WorldMaps category.
bool goblin::goods_is_map(int32_t goods_id)
{
    const int sg = goods_sort_group(goods_id);
    return sg == 190 || sg == 191;
}

// Live category fallback for the disk-loot path — Phase-3 TAIL classifier. item_marker_category()
// now classifies the bulk in confidence (non-goods key-range + curated exceptions + ER's live
// (goodsType, sortGroupId) taxonomy) and returns -1 only for the DEFAULT/catch-all tail. This owns
// that tail so the marker still shows, AND flags it as live_classified so the [ITEMCLASS] census /
// docs/item_classification.md track exactly the uncertain surface (the catch-all bucket). Returns -1
// only when the item is genuinely unknown.
int goblin::classify_item_live(int32_t key)
{
    using C = goblin::generated::Category;
    if (key <= 0) return -1;
    if (key >= 500000000) // goods — only the default/catch-all tail reaches here
    {
        const int32_t gid = key - 500000000;
        const int sg = goods_sort_group(gid);
        // Key/important-item tail (medallions, keys, Needles, quest goods) that ER's taxonomy leaves
        // without a fine sub-bucket → the QuestProgression ("Key Items") category, NOT the Crafting
        // Materials catch-all. Keyed on goodsType 1 = key/important item. Maps are goodsType 1 too but
        // owned by the dedicated WorldMaps pass — never re-bucket them here (sortGroupId 190/191).
        if (sg != 190 && sg != 191 && goods_type_live(gid) == 1)
            return (int)C::QuestProgression;
        return (int)C::LootCraftingMaterials;  // generic catch-all (currencies, misc gather, ...)
    }
    // Non-goods are fully resolved by item_marker_category's key-range; kept as a defensive fallback.
    if (key >= 400000000) return (int)C::EquipAshesOfWar;   // gem
    if (key >= 300000000) return (int)C::EquipTalismans;    // accessory
    if (key >= 200000000) return (int)C::EquipArmour;       // protector
    if (key >= 100000000)
        return (key - 100000000 >= 50000000) ? (int)C::LootAmmo : (int)C::EquipArmaments;
    return -1;
}
