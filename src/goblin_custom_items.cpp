#include "goblin_custom_items.hpp"

#include "goblin_messages.hpp"     // inject_fmg_entries, FmgEntry
#include "goblin_param_edit.hpp"   // param_clone_row, param_set_field_by_name
#include "goblin_sidecar.hpp"      // sidecar::add_custom_item

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define TOML_EXCEPTIONS 0          // parse errors returned as a result, not thrown
#include <toml++/toml.hpp>

namespace goblin::custom_items
{
namespace
{
// One item category → its param table, item-id encoding (category bits OR'd onto the row id), and
// the BASE name FMG slot the item-name UI renders from (windows_fmg_slot_re_findings.md). Grantable
// row-id ceiling is 0x7FFFFE (give_item no-ops above it) — enforced per item below.
struct Category
{
    const char   *key;        // TOML array-of-tables key
    const wchar_t *param;     // EquipParam* table
    uint32_t      id_base;    // category encoding for give/count/sidecar (goods=0x40000000, …)
    uint32_t      name_slot;  // base FMG slot for the name
};

constexpr Category kCategories[] = {
    {"goods",     L"EquipParamGoods",     0x40000000u, 10},
    {"weapon",    L"EquipParamWeapon",    0x00000000u, 11},
    {"protector", L"EquipParamProtector", 0x10000000u, 12},
    {"accessory", L"EquipParamAccessory", 0x20000000u, 13},
};

constexpr int32_t kGrantCeiling = 0x7FFFFE;  // max grantable row id (goods 23-bit field)

std::wstring utf8_to_wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// A toml value in the `fields` map is an int or a float; params take a double.
bool node_as_double(const toml::node &n, double &out)
{
    if (auto *i = n.as_integer())       { out = (double)i->get(); return true; }
    if (auto *f = n.as_floating_point()) { out = f->get(); return true; }
    return false;
}

// Apply one item record within a category. Returns true if it was defined + registered.
bool apply_item(const Category &cat, const toml::table &item, size_t idx)
{
    auto id_opt = item["id"].value<int64_t>();
    if (!id_opt)
    {
        spdlog::error("[CUSTOMITEM] {}[{}]: missing/invalid 'id' — skipped", cat.key, idx);
        return false;
    }
    int64_t id = *id_opt;
    if (id <= 0 || id > kGrantCeiling)
    {
        spdlog::error("[CUSTOMITEM] {}[{}]: id {} out of grantable range (1..0x{:X}) — skipped",
                      cat.key, idx, id, kGrantCeiling);
        return false;
    }

    auto clone_opt = item["clone"].value<int64_t>();
    if (!clone_opt)
    {
        spdlog::error("[CUSTOMITEM] {} id={}: missing 'clone' template row — skipped", cat.key, id);
        return false;
    }

    // 1) DEFINE: clone the template row (mod-agnostic — the ACTIVE install's own row).
    if (!goblin::paramedit::param_clone_row(cat.param, (uint64_t)*clone_opt, (int32_t)id))
    {
        spdlog::error("[CUSTOMITEM] {} id={}: clone from {} failed (src missing / id collision) — skipped",
                      cat.key, id, *clone_opt);
        return false;
    }

    // 2) fields (optional): set each by name.
    int fields_set = 0;
    if (auto *fields = item["fields"].as_table())
    {
        for (auto &&[fname, fnode] : *fields)
        {
            double v = 0;
            std::string field(fname.str());
            if (!node_as_double(fnode, v))
            {
                spdlog::warn("[CUSTOMITEM] {} id={}: field '{}' is not a number — skipped",
                             cat.key, id, field);
                continue;
            }
            if (goblin::paramedit::param_set_field_by_name(cat.param, (uint64_t)id, field.c_str(), v))
                ++fields_set;
            else
                spdlog::warn("[CUSTOMITEM] {} id={}: field '{}' not set (unknown / OOR)",
                             cat.key, id, field);
        }
    }

    // 3) name (optional): inject at the BASE name slot the item-name UI renders from.
    std::string name = item["name"].value_or(std::string{});
    if (!name.empty())
    {
        if (!goblin::inject_fmg_entries(cat.name_slot, {{(int32_t)id, utf8_to_wide(name)}}))
            spdlog::warn("[CUSTOMITEM] {} id={}: name inject at slot {} failed",
                         cat.key, id, cat.name_slot);
    }

    // 4) register with the sidecar as a DECLARATIVE author item (category-encoded id) → granted on
    // world-enter, stripped from the vanilla save, but NOT persisted to the .mfg (re-applied from the
    // toml every boot). qty optional (default 1).
    int64_t qty = item["qty"].value_or<int64_t>(1);
    uint32_t encoded = cat.id_base | (uint32_t)id;
    goblin::sidecar::register_author_item(encoded, (int32_t)qty);

    spdlog::info("[CUSTOMITEM] {} id={} (enc {:#010x}) cloned<-{} fields={} name='{}' qty={} registered",
                 cat.key, id, encoded, *clone_opt, fields_set, name, qty);
    return true;
}
}  // namespace

int apply(const std::filesystem::path &mod_folder)
{
    std::filesystem::path path = mod_folder / "custom_items.toml";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return 0;  // no author file — nothing to do

    auto result = toml::parse_file(path.string());
    if (!result)
    {
        const auto &err = result.error();
        spdlog::error("[CUSTOMITEM] parse error in {}: {} (line {})", path.string(),
                      std::string(err.description()), err.source().begin.line);
        return 0;
    }
    const toml::table &root = result.table();

    int applied = 0;
    for (const auto &cat : kCategories)
    {
        auto *arr = root[cat.key].as_array();
        if (!arr) continue;
        size_t idx = 0;
        for (auto &&node : *arr)
        {
            if (auto *item = node.as_table())
                applied += apply_item(cat, *item, idx) ? 1 : 0;
            else
                spdlog::error("[CUSTOMITEM] {}[{}]: not a table — skipped", cat.key, idx);
            ++idx;
        }
    }
    spdlog::info("[CUSTOMITEM] applied {} custom item(s) from {}", applied, path.string());
    return applied;
}
}  // namespace goblin::custom_items
