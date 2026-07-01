#include "name_fmg_en.hpp"

#include "loot_disk.hpp"  // read_loose_file_decompressed / read_game_file_decompressed

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

// English name index built from the engus Name FMGs on disk. See the header for
// why (GetMessage is language-locked; disk read is mod-agnostic + English).
//
// LAYERING (this was a real bug): read_game_file_decompressed falls back from a
// mod's loose file to the PACKED VANILLA dvdbnd, so a mod that only overrides
// some msgbnds (e.g. ERR ships a single merged loose item_dlc02) would otherwise
// mix vanilla + mod FMGs, and vanilla's differently-ranged PlaceName mislabels
// mod ids (id 100000 -> "Godrick the Grafted" instead of "Royal Gardens"). So we
// resolve in two passes: LOOSE (mod) bundles first — authoritative, they win —
// then PACKED bundles only fill ids the mod didn't provide.
//
// Binary layout references (proven elsewhere in this codebase):
//   BND4 container   — tools/list_fmgs.py parse_bnd4
//   FMG v2 (packed)  — goblin_messages.cpp (FmgGroup = 16 bytes)
namespace
{
    inline uint32_t rd32(const uint8_t *p, size_t n, size_t off)
    {
        if (off + 4 > n) return 0;
        uint32_t v; std::memcpy(&v, p + off, 4); return v;
    }
    inline int32_t rdi32(const uint8_t *p, size_t n, size_t off)
    {
        return static_cast<int32_t>(rd32(p, n, off));
    }
    inline uint64_t rd64(const uint8_t *p, size_t n, size_t off)
    {
        if (off + 8 > n) return 0;
        uint64_t v; std::memcpy(&v, p + off, 8); return v;
    }

    // English-name categories the search needs. Enemies are intentionally absent
    // — enemy-drop labels resolve live via NpcParam.nameId → NpcName (+700M path).
    enum class Cat { None, Goods, Weapon, Protector, Accessory, Gem, Npc, Place };
    constexpr size_t kCatCount = 8;

    // Merged English index: per category, real-id → UTF-8 English name. Written
    // once by load_english_name_index(), read-only after (no lock needed as long
    // as load runs to completion before the first lookup — dllmain guarantees it).
    std::array<std::unordered_map<int32_t, std::string>, kCatCount> g_index;
    bool g_loaded = false;

    Cat cat_for_fmg_name(const std::string &basename)
    {
        struct { const char *prefix; Cat cat; } kMap[] = {
            {"GoodsName",     Cat::Goods},
            {"WeaponName",    Cat::Weapon},
            {"ProtectorName", Cat::Protector},
            {"AccessoryName", Cat::Accessory},
            {"GemName",       Cat::Gem},
            {"ArtsName",      Cat::Gem},   // Ash-of-War arts share the +400M gem range
            {"NpcName",       Cat::Npc},
            {"PlaceName",     Cat::Place},
        };
        for (const auto &m : kMap)
            if (basename.rfind(m.prefix, 0) == 0)
                return m.cat;
        return Cat::None;
    }

    Cat decode(int32_t id, int32_t &real_id)
    {
        real_id = id;
        if (id >= 1600000000 && id < 1700000000) { real_id = id - 700000000; return Cat::Npc; }
        if (id >=  700000000 && id <  800000000) { real_id = id - 700000000; return Cat::Npc; }
        if (id >=  500000000 && id <  600000000) { real_id = id - 500000000; return Cat::Goods; }
        if (id >=  400000000 && id <  500000000) { real_id = id - 400000000; return Cat::Gem; }
        if (id >=  300000000 && id <  400000000) { real_id = id - 300000000; return Cat::Accessory; }
        if (id >=  200000000 && id <  300000000) { real_id = id - 200000000; return Cat::Protector; }
        if (id >=  100000000 && id <  200000000) { real_id = id - 100000000; return Cat::Weapon; }
        if (id >=   50000000 && id <  100000000) { real_id = id;             return Cat::Weapon; } // ammo (as-is)
        if (id >= 0 && id < 50000000)            { real_id = id;             return Cat::Place; }  // boss/place
        return Cat::None;
    }

    std::string utf16le_to_utf8(const uint8_t *d, size_t n, size_t str_off)
    {
        std::wstring w;
        for (size_t p = str_off; p + 1 < n; p += 2)
        {
            uint16_t c = static_cast<uint16_t>(d[p] | (static_cast<uint16_t>(d[p + 1]) << 8));
            if (c == 0) break;
            w.push_back(static_cast<wchar_t>(c));
        }
        if (w.empty()) return {};
        int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (need <= 1) return {};
        std::string s(static_cast<size_t>(need - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], need, nullptr, nullptr);
        return s;
    }

    // Materialize every id→string in one FMG (packed v2, disk form: offsets are
    // FMG-relative) into `cat`'s map. `overwrite` = later/authoritative source
    // replaces an existing id; false = keep the first (loose) value.
    void index_fmg(const uint8_t *d, size_t n, Cat cat, bool overwrite)
    {
        if (n < 0x28) return;
        uint32_t entry_count = rd32(d, n, 0x10);
        uint64_t str_off_tbl = rd64(d, n, 0x18);
        uint32_t group_count = rd32(d, n, 0x0C);
        auto &m = g_index[static_cast<size_t>(cat)];
        for (uint32_t g = 0; g < group_count; ++g)
        {
            // FMG v2 group entry = 4×int32 = 16 bytes (matches FmgGroup in
            // goblin_messages.cpp; list_fmgs.py's 0x18 stride is wrong).
            size_t off = 0x28 + static_cast<size_t>(g) * 0x10;
            if (off + 12 > n) break;
            int32_t idx_start = rdi32(d, n, off);
            int32_t id_start  = rdi32(d, n, off + 4);
            int32_t id_end    = rdi32(d, n, off + 8);
            if (id_end < id_start) continue;
            for (int64_t id = id_start; id <= id_end; ++id)
            {
                if (!overwrite && m.count(static_cast<int32_t>(id))) continue;
                int64_t entry_idx = static_cast<int64_t>(idx_start) + (id - id_start);
                if (entry_idx < 0 || static_cast<uint64_t>(entry_idx) >= entry_count) continue;
                size_t soff = static_cast<size_t>(str_off_tbl) + static_cast<size_t>(entry_idx) * 8;
                if (soff + 8 > n) continue;
                uint64_t str_off = rd64(d, n, soff);
                if (str_off == 0 || str_off + 2 > n) continue;
                std::string s = utf16le_to_utf8(d, n, static_cast<size_t>(str_off));
                if (s.empty()) continue;
                if (overwrite) m[static_cast<int32_t>(id)] = std::move(s);
                else           m.emplace(static_cast<int32_t>(id), std::move(s));
            }
        }
    }

    // Unpack a decompressed BND4 (msgbnd) and index each Name-category FMG.
    int index_msgbnd(const std::vector<uint8_t> &buf, bool overwrite)
    {
        const uint8_t *d = buf.data();
        size_t n = buf.size();
        if (n < 0x40 || std::memcmp(d, "BND4", 4) != 0) return 0;
        int32_t file_count = rdi32(d, n, 0x0C);
        int64_t hdr_size   = static_cast<int64_t>(rd64(d, n, 0x20));
        if (file_count <= 0 || file_count > 100000 || hdr_size < 0x24) return 0;
        int kept = 0;
        for (int i = 0; i < file_count; ++i)
        {
            size_t eoff = 0x40 + static_cast<size_t>(i) * static_cast<size_t>(hdr_size);
            if (eoff + 0x24 > n) break;
            int64_t uncomp   = static_cast<int64_t>(rd64(d, n, eoff + 0x10));
            int32_t data_off = rdi32(d, n, eoff + 0x18);
            int32_t name_off = rdi32(d, n, eoff + 0x20);
            if (data_off < 0 || uncomp <= 0) continue;
            if (static_cast<size_t>(data_off) + static_cast<size_t>(uncomp) > n) continue;

            std::string base;  // basename after the last '\' or '/'
            if (name_off > 0 && static_cast<size_t>(name_off) + 1 < n)
            {
                for (size_t j = static_cast<size_t>(name_off); j + 1 < n; j += 2)
                {
                    uint16_t c = static_cast<uint16_t>(d[j] | (static_cast<uint16_t>(d[j + 1]) << 8));
                    if (c == 0) break;
                    if (c == L'\\' || c == L'/') base.clear();
                    else base.push_back(static_cast<char>(c & 0xFF));
                }
            }
            Cat cat = cat_for_fmg_name(base);
            if (cat == Cat::None) continue;
            index_fmg(d + data_off, static_cast<size_t>(uncomp), cat, overwrite);
            ++kept;
        }
        return kept;
    }
}

namespace goblin
{
    void load_english_name_index()
    {
        if (g_loaded) return;  // idempotent
        g_loaded = true;

        // ALWAYS engus, regardless of the game's active language — that's the point.
        static const char *kBundles[] = {
            "msg/engus/item.msgbnd.dcx",
            "msg/engus/item_dlc01.msgbnd.dcx",
            "msg/engus/item_dlc02.msgbnd.dcx",
            "msg/engus/menu.msgbnd.dcx",
            "msg/engus/menu_dlc01.msgbnd.dcx",
            "msg/engus/menu_dlc02.msgbnd.dcx",
        };

        // Pass 1 — LOOSE (mod) bundles are authoritative; later ones overwrite.
        int loose_bundles = 0, loose_fmgs = 0;
        for (const char *rel : kBundles)
        {
            std::vector<uint8_t> buf = goblin::worldmap::read_loose_file_decompressed(rel);
            if (buf.size() < 0x40) continue;
            int k = index_msgbnd(buf, /*overwrite=*/true);
            if (k > 0) { ++loose_bundles; loose_fmgs += k; }
        }

        // Pass 2 — PACKED (dvdbnd) fills only ids the mod didn't provide. On a
        // pure-vanilla install pass 1 is empty and this supplies everything.
        int packed_bundles = 0, packed_fmgs = 0;
        for (const char *rel : kBundles)
        {
            std::vector<uint8_t> buf = goblin::worldmap::read_game_file_decompressed(rel);
            if (buf.size() < 0x40) continue;
            int k = index_msgbnd(buf, /*overwrite=*/false);
            if (k > 0) { ++packed_bundles; packed_fmgs += k; }
        }

        size_t total = 0;
        for (auto &m : g_index) total += m.size();
        if (total == 0)
            spdlog::warn("[NAMEEN] no English name FMGs found on disk — F1 search "
                         "falls back to game-language matching");
        else
            spdlog::info("[NAMEEN] English index: {} names "
                         "(loose {} FMGs/{} bundles, packed-fill {} FMGs/{} bundles)",
                         total, loose_fmgs, loose_bundles, packed_fmgs, packed_bundles);
    }

    std::string lookup_name_en_disk_utf8(int32_t encoded_id)
    {
        if (!g_loaded) return {};
        int32_t real_id = 0;
        Cat cat = decode(encoded_id, real_id);
        if (cat == Cat::None) return {};
        const auto &m = g_index[static_cast<size_t>(cat)];
        auto it = m.find(real_id);
        return it == m.end() ? std::string{} : it->second;
    }
}
