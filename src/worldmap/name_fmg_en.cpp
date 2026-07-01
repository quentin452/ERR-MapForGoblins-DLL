#include "name_fmg_en.hpp"

#include "loot_disk.hpp"  // goblin::worldmap::read_game_file_decompressed

#include <array>
#include <cstring>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

// English name index built from the engus Name FMGs on disk. See the header for
// why (GetMessage is language-locked; disk read is mod-agnostic + English).
//
// Binary layout references (already documented/proven in this codebase):
//   BND4 container   — tools/list_fmgs.py parse_bnd4
//   FMG v2 (packed)  — goblin_messages.cpp patch_fmg_in_memory + list_fmgs.py
namespace
{
    // Little-endian scalar reads with a bounds guard (returns 0 past the end;
    // callers separately validate ranges before trusting a value).
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

    // The English-name categories the search needs. Enemies are intentionally
    // absent — enemy-drop labels resolve live via NpcParam.nameId → NpcName
    // (the +700M NPC path), so there is no separate Codex/TutorialTitle source.
    enum class Cat { None, Goods, Weapon, Protector, Accessory, Gem, Npc, Place };

    // Retained FMG byte-buffers per category (base + any DLC layers). Each is a
    // whole FMG copied out of a decompressed msgbnd; searched on demand. Written
    // once by load_english_name_index(), read-only after (no lock needed as long
    // as load runs to completion before the first lookup — dllmain guarantees it).
    std::array<std::vector<std::vector<uint8_t>>, 8> g_fmgs;
    bool g_loaded = false;

    // Map an embedded FMG file name (e.g. "GoodsName.fmg", "GoodsName_dlc02.fmg")
    // to its category. Prefix-match on the "<X>Name" token so DLC-suffixed copies
    // fold into the same bucket, and "GoodsInfo"/"GoodsCaption"/… never match.
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

    // Decode an encoded marker name_id into (category, real id within that FMG).
    // Mirrors decode_textid (goblin_messages.cpp) but only for the bands the
    // alias table ever carried: items, NPCs, and raw place/boss names.
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
        return Cat::None;  // negative / out of every band
    }

    // Search one FMG (packed v2, disk form) for an id → UTF-16LE string. String
    // offsets are FMG-relative on disk (not fixed up to absolute like the live
    // repo), so use them directly. Returns false on miss / gap / any bad range.
    bool fmg_find(const uint8_t *d, size_t n, int32_t target, std::wstring &out)
    {
        if (n < 0x28) return false;
        uint32_t entry_count  = rd32(d, n, 0x10);
        uint64_t str_off_tbl  = rd64(d, n, 0x18);
        uint32_t group_count  = rd32(d, n, 0x0C);
        for (uint32_t g = 0; g < group_count; ++g)
        {
            // FMG v2 group entry = 4×int32 = 16 bytes (string_index, first_id,
            // last_id, pad). Matches FmgGroup in goblin_messages.cpp.
            size_t off = 0x28 + static_cast<size_t>(g) * 0x10;
            if (off + 12 > n) break;
            int32_t idx_start = rdi32(d, n, off);
            int32_t id_start  = rdi32(d, n, off + 4);
            int32_t id_end    = rdi32(d, n, off + 8);
            if (target < id_start || target > id_end) continue;
            int64_t entry_idx = static_cast<int64_t>(idx_start) + (target - id_start);
            if (entry_idx < 0 || static_cast<uint64_t>(entry_idx) >= entry_count) return false;
            size_t soff = static_cast<size_t>(str_off_tbl) + static_cast<size_t>(entry_idx) * 8;
            if (soff + 8 > n) return false;
            uint64_t str_off = rd64(d, n, soff);
            if (str_off == 0 || str_off + 2 > n) return false;
            out.clear();
            for (size_t p = static_cast<size_t>(str_off); p + 1 < n; p += 2)
            {
                uint16_t c = static_cast<uint16_t>(d[p] | (static_cast<uint16_t>(d[p + 1]) << 8));
                if (c == 0) break;
                out.push_back(static_cast<wchar_t>(c));
            }
            return !out.empty();
        }
        return false;
    }

    // Walk a decompressed BND4 (msgbnd) and copy each Name-category FMG into
    // g_fmgs. Only the small *Name FMGs are retained; captions/info/etc. and the
    // empty DLC stubs are ignored, so the resident footprint stays ~1-2 MB.
    void index_msgbnd(const std::vector<uint8_t> &buf)
    {
        const uint8_t *d = buf.data();
        size_t n = buf.size();
        if (n < 0x40 || std::memcmp(d, "BND4", 4) != 0) return;
        int32_t file_count = rdi32(d, n, 0x0C);
        int64_t hdr_size   = static_cast<int64_t>(rd64(d, n, 0x20));
        if (file_count <= 0 || file_count > 100000 || hdr_size < 0x24) return;
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

            // Entry name is a UTF-16LE path; take the basename after the last
            // '\' or '/'. Only the bytes up to the first NUL matter.
            std::string base;
            if (name_off > 0 && static_cast<size_t>(name_off) + 1 < n)
            {
                for (size_t j = static_cast<size_t>(name_off); j + 1 < n; j += 2)
                {
                    uint16_t c = static_cast<uint16_t>(d[j] | (static_cast<uint16_t>(d[j + 1]) << 8));
                    if (c == 0) break;
                    char ch = (c == L'\\' || c == L'/') ? '\0' : static_cast<char>(c & 0xFF);
                    if (ch == '\0') base.clear();  // reset at each separator → keep basename
                    else base.push_back(ch);
                }
            }
            Cat cat = cat_for_fmg_name(base);
            if (cat == Cat::None) continue;
            std::vector<uint8_t> fmg(d + data_off, d + data_off + static_cast<size_t>(uncomp));
            g_fmgs[static_cast<size_t>(cat)].push_back(std::move(fmg));
            ++kept;
        }
        spdlog::info("[NAMEEN] msgbnd indexed: {} name FMG(s)", kept);
    }
}

namespace goblin
{
    void load_english_name_index()
    {
        if (g_loaded) return;  // idempotent
        g_loaded = true;

        // Read every candidate engus msgbnd that exists; layouts differ by mod
        // (ERR bundles everything in item_dlc02; vanilla splits item/menu + DLC).
        // Missing files are skipped silently (read returns empty). ALWAYS engus,
        // regardless of the game's active language — that's the point.
        static const char *kBundles[] = {
            "msg/engus/item.msgbnd.dcx",
            "msg/engus/item_dlc01.msgbnd.dcx",
            "msg/engus/item_dlc02.msgbnd.dcx",
            "msg/engus/menu.msgbnd.dcx",
            "msg/engus/menu_dlc01.msgbnd.dcx",
            "msg/engus/menu_dlc02.msgbnd.dcx",
        };
        int bundles = 0;
        for (const char *rel : kBundles)
        {
            std::vector<uint8_t> buf = goblin::worldmap::read_game_file_decompressed(rel);
            if (buf.size() < 0x40) continue;
            index_msgbnd(buf);
            ++bundles;
        }

        size_t total = 0;
        for (auto &v : g_fmgs) total += v.size();
        if (total == 0)
            spdlog::warn("[NAMEEN] no English name FMGs found on disk — F1 search "
                         "falls back to game-language matching");
        else
            spdlog::info("[NAMEEN] English name index ready: {} FMG buffer(s) from {} bundle(s)",
                         total, bundles);
    }

    std::string lookup_name_en_disk_utf8(int32_t encoded_id)
    {
        if (!g_loaded) return {};
        int32_t real_id = 0;
        Cat cat = decode(encoded_id, real_id);
        if (cat == Cat::None) return {};
        std::wstring w;
        bool hit = false;
        for (const auto &fmg : g_fmgs[static_cast<size_t>(cat)])
            if (fmg_find(fmg.data(), fmg.size(), real_id, w)) { hit = true; break; }
        if (!hit || w.empty()) return {};
        // Drop control/markup lines the way the baked generator did ("<" filter);
        // FMG name strings shouldn't contain tags, but be defensive.
        int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (need <= 1) return {};
        std::string s(static_cast<size_t>(need - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], need, nullptr, nullptr);
        return s;
    }
}
