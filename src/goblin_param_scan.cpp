#include "goblin_param_scan.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

#include "from/params.hpp"
#include "goblin_config.hpp"

// [PARAMSCAN] — generic in-process param needle search (see the header). The scan is
// byte-stride (params are packed structs; ids are usually 4-aligned but not guaranteed),
// one-shot at init, over every row of every registered table — tens of MB, well under a
// second in-process. Hits are capped per table so a coincidental-value flood (a needle
// that happens to be a common item id) can't drown the log; the per-table summary line
// still reports the TRUE total, so a capped table is visible as such.

namespace
{
// Group-2 landscape RE (docs/re/windows_group2_landscape_re_findings.md): Smithing Table
// binds ActionButtonParam 6250, Elevator ("Descend") binds 5010 — but neither co-occurs
// with its asset in EMEVD args, so the binding must live in a PARAM row (ObjAct family).
// This scan finds every table+offset that stores those ids.
constexpr uint32_t kNeedles[] = {6250, 5010};
constexpr int kMaxHitLinesPerTable = 40;

// SEH note: called via dllmain's safe_init_step (__try around a fn-pointer call —
// the preserved shape), so a stale/half-loaded table faults into that guard.
void scan_table(const char *name, from::params::ParamTable *table)
{
    auto *base = reinterpret_cast<uint8_t *>(table);
    int lines = 0, total = 0;
    uint64_t row_size = 0;
    for (uint16_t r = 0; r < table->num_rows; r++)
    {
        const auto &ri = table->rows[r];
        if (ri.param_end_offset <= ri.param_offset) continue;
        const uint64_t len = ri.param_end_offset - ri.param_offset;
        if (len > 0x10000) continue;  // implausible row — corrupt/foreign table layout
        if (!row_size) row_size = len;
        const uint8_t *p = base + ri.param_offset;
        for (uint64_t off = 0; off + 4 <= len; off++)
        {
            uint32_t v;
            std::memcpy(&v, p + off, 4);
            for (uint32_t needle : kNeedles)
            {
                if (v != needle) continue;
                total++;
                if (lines < kMaxHitLinesPerTable)
                {
                    lines++;
                    spdlog::info("[PARAMSCAN] {} row {} +0x{:X} = {}", name,
                                 (unsigned long long)ri.row_id, off, v);
                }
            }
        }
    }
    if (total)
        spdlog::info("[PARAMSCAN] {}: {} hit(s) across {} rows (row size {}){}", name,
                     total, table->num_rows, row_size,
                     total > kMaxHitLinesPerTable ? "  [hit lines capped]" : "");
}
} // namespace

namespace goblin
{
void param_needle_scan()
{
    if (!goblin::config::debugLogging) return;
    auto *param_list = *from::params::param_list_address;
    if (!param_list)
    {
        spdlog::warn("[PARAMSCAN] param list not resolved — scan skipped");
        return;
    }
    spdlog::info("[PARAMSCAN] scanning all param tables for u32 needles: 6250 (Smithing "
                 "Table ABP), 5010 (Elevator ABP)");
    constexpr size_t kEntries =
        sizeof(param_list->entries) / sizeof(param_list->entries[0]);
    for (size_t i = 0; i < kEntries; i++)
    {
        auto *prc = param_list->entries[i].param_res_cap;
        if (!prc || !prc->param_header || !prc->param_header->param_table) continue;
        const std::string name = from::params::internal::wstring_to_string(
            from::params::dlw_c_str(&prc->param_name));
        scan_table(name.c_str(), prc->param_header->param_table);
    }
    spdlog::info("[PARAMSCAN] done");
}
} // namespace goblin
