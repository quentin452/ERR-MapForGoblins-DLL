#include "goblin_param_overrides.hpp"

#include <filesystem>
#include <string>

#include <mini/ini.h>
#include <spdlog/spdlog.h>

#include "goblin_config.hpp"       // config::paramOverrides, config_ini_path()
#include "goblin_param_edit.hpp"   // param_set_field_by_name / field_is_known

namespace
{
namespace fs = std::filesystem;

// Split "Param:rowId:fieldName:value" into its 4 parts. false if malformed. NB the edit spec lives
// in the ini VALUE, NOT the key — mINI LOWERCASES keys/section-names on read, which would destroy the
// case-sensitive param/field identifiers; ini values keep their case.
bool parse_spec(const std::string &spec, std::string &param, uint64_t &row, std::string &field,
                double &value)
{
    size_t c1 = spec.find(':');
    size_t c2 = c1 == std::string::npos ? c1 : spec.find(':', c1 + 1);
    size_t c3 = c2 == std::string::npos ? c2 : spec.find(':', c2 + 1);
    if (c1 == std::string::npos || c2 == std::string::npos || c3 == std::string::npos) return false;
    param = spec.substr(0, c1);
    std::string row_s = spec.substr(c1 + 1, c2 - c1 - 1);
    field = spec.substr(c2 + 1, c3 - c2 - 1);
    std::string val_s = spec.substr(c3 + 1);
    if (param.empty() || row_s.empty() || field.empty() || val_s.empty()) return false;
    try
    {
        row = std::stoull(row_s, nullptr, 0);  // 0x.. auto-detected
        value = std::stod(val_s);
    }
    catch (...)
    {
        return false;
    }
    return true;
}
}  // namespace

int goblin::apply_param_overrides()
{
    if (!goblin::config::paramOverrides)
        return 0;  // opt-in only; silent when off (the section comment documents it)

    fs::path ini = goblin::config_ini_path();
    if (ini.empty())
    {
        spdlog::warn("[PARAMOVR] enabled but no config path known — skipped");
        return 0;
    }
    fs::path ovr = ini.parent_path() / "param_overrides.ini";
    std::error_code ec;
    if (!fs::exists(ovr, ec))
    {
        spdlog::info("[PARAMOVR] enabled but {} not found — nothing to apply", ovr.string());
        return 0;
    }

    mINI::INIFile file(ovr.string());
    mINI::INIStructure ini_data;
    if (!file.read(ini_data))
    {
        spdlog::warn("[PARAMOVR] failed to read {}", ovr.string());
        return 0;
    }

    int applied = 0, skipped = 0;
    // Each edit is one ini VALUE "Param:row:field:value" (the key is just a label). Tolerate entries
    // in any section (iterate all) — [Overrides] is the documented one.
    for (auto const &section : ini_data)
    {
        for (auto const &kv : section.second)
        {
            const std::string &spec = kv.second;  // "Param:row:field:value"
            std::string param, field;
            uint64_t row = 0;
            double value = 0;
            if (!parse_spec(spec, param, row, field, value))
            {
                spdlog::warn("[PARAMOVR] bad entry '{}' (want Param:rowId:fieldName:value) — skipped",
                             spec);
                ++skipped;
                continue;
            }
            std::wstring wparam(param.begin(), param.end());  // ASCII param names
            if (!goblin::paramedit::field_is_known(wparam.c_str(), field.c_str()))
            {
                spdlog::warn("[PARAMOVR] {}: field not in registry (add a FieldSpec AOB) — skipped",
                             spec);
                ++skipped;
                continue;
            }
            if (goblin::paramedit::param_set_field_by_name(wparam.c_str(), row, field.c_str(), value))
            {
                spdlog::info("[PARAMOVR] applied {}:{}:{} = {}", param, row, field, value);
                ++applied;
            }
            else
            {
                spdlog::warn("[PARAMOVR] {}: write failed (row missing / offset OOR / fault) — skipped",
                             spec);
                ++skipped;
            }
        }
    }
    spdlog::info("[PARAMOVR] done: {} applied, {} skipped ({})", applied, skipped, ovr.string());
    return applied;
}
