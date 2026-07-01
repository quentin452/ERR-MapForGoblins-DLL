#include "goblin_param_scan.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "from/params.hpp"
#include "goblin_config.hpp"
#include "goblin_messages.hpp"            // goblin::raw_message_utf8 (ABPTEXT probe)
#include "goblin_overlay_render_api.hpp"  // overlay_api::disk_loot_dir()
#include "worldmap/msbe_parser.hpp"       // goblin::msbe::dcx_decompress

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

// Tables to dump RAW (row id + hex) into logs/paramdump_<name>.txt for offline
// analysis — the needle scan proved no param table stores the Group-2 ABP ids
// as a field VALUE, so the next question is what ObjActParam/ActionButtonParam
// rows actually contain (row IDS are in the descriptors, not the data — a
// needle scan can't see them). regulation.bin is encrypted on disk; the live
// tables are the easiest clean source, then python the .txt on this machine.
constexpr const char *kDumpTables[] = {"ObjActParam", "ActionButtonParam"};

// Directory of the DLL itself (same trick as goblin_crashdump) → logs/ next to it.
std::filesystem::path own_logs_dir()
{
    HMODULE self = nullptr;
    wchar_t buf[MAX_PATH] = {0};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&own_logs_dir), &self) &&
        GetModuleFileNameW(self, buf, MAX_PATH))
        return std::filesystem::path(buf).parent_path() / "logs";
    return {};
}

void dump_table(const char *name, from::params::ParamTable *table)
{
    const auto dir = own_logs_dir();
    if (dir.empty()) return;
    const auto path = dir / (std::string("paramdump_") + name + ".txt");
    FILE *f = _wfopen(path.wstring().c_str(), L"wb");
    if (!f)
    {
        spdlog::warn("[PARAMSCAN] dump: cannot open {}", path.string());
        return;
    }
    auto *base = reinterpret_cast<uint8_t *>(table);
    uint64_t row_size = 0;
    for (uint16_t r = 0; r < table->num_rows; r++)
    {
        const auto &ri = table->rows[r];
        uint64_t len;
        if (r + 1 < table->num_rows && table->rows[r + 1].param_offset > ri.param_offset)
            len = table->rows[r + 1].param_offset - ri.param_offset;
        else if (row_size)
            len = row_size;
        else if (ri.param_end_offset > ri.param_offset)
            len = ri.param_end_offset - ri.param_offset;
        else
            continue;
        if (len > 0x1000) continue;
        if (!row_size) row_size = len;
        std::fprintf(f, "%llu :", (unsigned long long)ri.row_id);
        const uint8_t *p = base + ri.param_offset;
        for (uint64_t b = 0; b < len; b++) std::fprintf(f, " %02X", p[b]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    spdlog::info("[PARAMSCAN] dumped {} ({} rows, row size {}) -> {}", name,
                 table->num_rows, row_size, path.string());
}

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
        // Row LENGTH = distance to the next row's data (rows are laid out
        // sequentially). param_end_offset is NOT the row end — it points far past
        // the row (first scan run attributed the whole table tail to every row:
        // "row size 65486" on tables whose real rows are a few hundred bytes,
        // duplicate hits in every following row). Last row: assume the uniform
        // stride every FromSoft param table has.
        uint64_t len;
        if (r + 1 < table->num_rows && table->rows[r + 1].param_offset > ri.param_offset)
            len = table->rows[r + 1].param_offset - ri.param_offset;
        else if (row_size)
            len = row_size;
        else if (ri.param_end_offset > ri.param_offset)
            len = ri.param_end_offset - ri.param_offset;
        else
            continue;
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

// ── [EMEVDSCAN] — needle search over EMEVD instruction ARGS ─────────────────
// The param dumps proved ABP 6250/5010 are referenced by NO param row — so the
// prompt must be requested by an EMEVD common template's BODY (the instantiation
// args only carry entities, which is why the recon's arg-probe saw no
// co-occurrence — exactly like Portal's 90005605). Scan every event/*.emevd.dcx's
// decompressed instruction-arg blobs for the needles and log the owning EVENT id:
// that id IS the lift/smithing template to harvest like Portal.
namespace
{
// Same resolver as loot_disk's (private there): the game imports oo2core, so the
// module is already in-process.
goblin::msbe::OodleDecompressFn emevd_oodle()
{
    static goblin::msbe::OodleDecompressFn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    HMODULE h = GetModuleHandleW(L"oo2core_6_win64.dll");
    if (!h) h = LoadLibraryW(L"oo2core_6_win64.dll");
    if (h) fn = (goblin::msbe::OodleDecompressFn)GetProcAddress(h, "OodleLZ_Decompress");
    return fn;
}

// The mod's event\ dir, derived from the discovered MapStudio dir (same sibling
// layouts loot_disk accepts). Empty until CreateFileW discovery has run.
std::filesystem::path emevd_dir()
{
    std::error_code ec;
    const auto ms = goblin::overlay_api::disk_loot_dir();
    if (ms.empty()) return {};
    if (auto e = ms.parent_path().parent_path() / "event"; std::filesystem::exists(e, ec)) return e;
    if (auto e = ms.parent_path() / "event"; std::filesystem::exists(e, ec)) return e;
    if (auto e = ms / "event"; std::filesystem::exists(e, ec)) return e;
    return {};
}

// Round-4: full-body dump of the interesting events (needle scans exhausted —
// 1030 has no per-lift inits at all, so its MECHANISM must be read from its
// instructions; 1042582000 is the generic (ABP,entity) prompt template whose
// exact arg-substitution slots we need). One line per instruction:
// bank[id] + raw arg hex → decoded offline against EMEDF.
// Round-5: 1030 decoded — it's the GLOBAL asset-prompt dispatcher (19 generic
// ABP waits incl. 5010, no entities — the asset↔prompt binding lives engine-
// side), so no per-lift harvest exists there. The two Siofra one-off events
// DO reference their lift entities though: dump them, grep the entity ids in
// the MSBs, and the lift AEG MODEL falls out.
constexpr uint64_t kBodyDumpEvents[] = {1030, 1042582000, 12022820, 12022822};

void dump_event_body(const std::filesystem::path &src, uint64_t eventId,
                     const uint8_t *buf, size_t len, size_t base, uint64_t instrCount,
                     uint64_t argsOff)
{
    const auto dir = own_logs_dir();
    if (dir.empty()) return;
    const auto path = dir / ("emevddump_" + std::to_string(eventId) + ".txt");
    FILE *f = _wfopen(path.wstring().c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "# %s event %llu (%llu instructions)\n", src.filename().string().c_str(),
                 (unsigned long long)eventId, (unsigned long long)instrCount);
    for (uint64_t j = 0; j < instrCount; ++j)
    {
        const size_t ins = base + (size_t)j * 0x20;
        if (ins + 0x20 > len) break;
        uint32_t bank, id;
        uint64_t argLen;
        int32_t argOff;
        std::memcpy(&bank, buf + ins + 0x00, 4);
        std::memcpy(&id, buf + ins + 0x04, 4);
        std::memcpy(&argLen, buf + ins + 0x08, 8);
        std::memcpy(&argOff, buf + ins + 0x10, 4);
        std::fprintf(f, "%llu: %u[%u] :", (unsigned long long)j, bank, id);
        if (argOff >= 0 && argLen <= 0x1000 && (size_t)argsOff + argOff + argLen <= len)
            for (uint64_t b = 0; b < argLen; b++)
                std::fprintf(f, " %02X", buf[(size_t)argsOff + argOff + b]);
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    spdlog::info("[EMEVDSCAN] dumped body of event {} ({}) -> {}", eventId,
                 src.filename().string(), path.string());
}

void emevd_scan_file(const std::filesystem::path &p, int &hit_lines)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> dcx((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
    std::vector<uint8_t> raw = goblin::msbe::dcx_decompress(dcx.data(), dcx.size(), nullptr,
                                                    emevd_oodle());
    const uint8_t *buf = raw.data();
    const size_t len = raw.size();
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return;
    auto rd32 = [&](size_t o) { uint32_t v; std::memcpy(&v, buf + o, 4); return v; };
    auto rd64 = [&](size_t o) { uint64_t v; std::memcpy(&v, buf + o, 8); return v; };
    auto inb = [&](size_t o, size_t n) { return o + n <= len; };
    const uint64_t eventCount = rd64(0x10), eventsOff = rd64(0x18);
    const uint64_t instrTblOff = rd64(0x28), argsOff = rd64(0x78);
    if (eventCount > 1000000u) return;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;
    for (uint64_t i = 0; i < eventCount; ++i)
    {
        const size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ)) break;
        const uint64_t eventId = rd64(e + 0x00);
        const uint64_t instrCount = rd64(e + 0x08), instrOffset = rd64(e + 0x10);
        if (instrCount > 1000000u) continue;
        const size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        for (uint64_t de : kBodyDumpEvents)
            if (eventId == de)
                dump_event_body(p, eventId, buf, len, base, instrCount, argsOff);
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            const size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ)) break;
            const uint32_t bank = rd32(ins + 0x00), id = rd32(ins + 0x04);
            const uint64_t argLen = rd64(ins + 0x08);
            const int32_t argOff = (int32_t)rd32(ins + 0x10);
            if (argOff < 0 || argLen < 4 || argLen > 0x10000) continue;
            const size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen)) continue;
            // Round-3 harvest: inits of known prompt templates, BOTH opcodes —
            // 2000[0] InitializeEvent(slot@0, eventId@4, args@8...) and
            // 2000[6] InitializeCommonEvent(eventId@0, args@4...) (round 2 showed
            // the lifts do NOT come in via 2000[0]; 1030's only [0]-init is
            // common.emevd's own with arg 0). 1030 = common lift event (3×
            // IfActionButton ABP 5010); 1042582000 = the generic (ABP, entity)
            // prompt template Roundtable's smithing init revealed.
            if (bank == 2000 && (id == 0 || id == 6) && argLen >= 8)
            {
                const uint64_t idOff = (id == 0) ? 4 : 0;
                const uint64_t argsStart = idOff + 4;
                const uint32_t initEventId = [&] { uint32_t v; std::memcpy(&v, buf + a + idOff, 4); return v; }();
                static constexpr uint32_t kHarvestTemplates[] = {1030, 1042582000};
                for (uint32_t ht : kHarvestTemplates)
                    if (initEventId == ht && hit_lines++ < 120)
                    {
                        std::string args;
                        for (uint64_t o = argsStart; o + 4 <= argLen && o < argsStart + 8 * 4; o += 4)
                        {
                            uint32_t v;
                            std::memcpy(&v, buf + a + o, 4);
                            args += " " + std::to_string(v);
                        }
                        spdlog::info("[EMEVDSCAN] {} 2000[{}] INIT of template {}: args{}",
                                     p.filename().string(), id, initEventId, args);
                    }
            }
            for (uint64_t off = 0; off + 4 <= argLen; off++)
            {
                uint32_t v;
                std::memcpy(&v, buf + a + off, 4);
                for (uint32_t needle : kNeedles)
                    if (v == needle && hit_lines++ < 120)
                    {
                        if (bank == 2000 && id == 0 && argLen >= 8)
                        {
                            // A needle passed as an INIT ARG: the template id (at arg+4)
                            // is the interesting part — log the whole init.
                            uint32_t slot, tmpl;
                            std::memcpy(&slot, buf + a + 0, 4);
                            std::memcpy(&tmpl, buf + a + 4, 4);
                            std::string args;
                            for (uint64_t o = 8; o + 4 <= argLen && o < 8 + 8 * 4; o += 4)
                            {
                                uint32_t av;
                                std::memcpy(&av, buf + a + o, 4);
                                args += " " + std::to_string(av);
                            }
                            spdlog::info("[EMEVDSCAN] {} event {} INIT template {} (slot {}) "
                                         "carries needle {} — args:{}",
                                         p.filename().string(), eventId, tmpl, slot, v, args);
                        }
                        else
                            spdlog::info("[EMEVDSCAN] {} event {} instr {}[{}] arg+0x{:X} = {}",
                                         p.filename().string(), eventId, bank, id, off, v);
                    }
            }
        }
    }
}

// ── [ABPTEXT] — resolve every ActionButtonParam row's prompt TEXT ────────────
// Ground truth (user, in-game): world smithing tables prompt "Use smithing
// table" — a DIFFERENT text from Roundtable's ABP 6250. Find the ABP-text FMG's
// physical slot (the one that resolves 6250's textId 7030), then dump
// (rowId, textId, text) for all 584 ABP rows → grep "smithing" offline gives
// the world-table ABP id = the next needle.
void abp_text_probe()
{
    auto *param_list = *from::params::param_list_address;
    if (!param_list) return;
    from::params::ParamTable *abp = nullptr;
    constexpr size_t kEntries = sizeof(param_list->entries) / sizeof(param_list->entries[0]);
    for (size_t i = 0; i < kEntries && !abp; i++)
    {
        auto *prc = param_list->entries[i].param_res_cap;
        if (!prc || !prc->param_header || !prc->param_header->param_table) continue;
        if (std::wstring_view(from::params::dlw_c_str(&prc->param_name)) == L"ActionButtonParam")
            abp = prc->param_header->param_table;
    }
    if (!abp) { spdlog::warn("[ABPTEXT] ActionButtonParam not found"); return; }
    auto *base = reinterpret_cast<uint8_t *>(abp);
    constexpr uint64_t kTextOff = 0x34;  // textId column (verified vs rows 6250/5010)
    // Slot discovery: which physical FMG slot resolves textId 7030 ("Smithing")?
    int slots[5];
    int nslots = 0;
    for (uint32_t slot = 0; slot < 200 && nslots < 5; slot++)
    {
        const std::string s = goblin::raw_message_utf8(slot, 7030);
        if (s.empty()) continue;
        spdlog::info("[ABPTEXT] slot {} resolves 7030 = \"{}\"", slot, s);
        slots[nslots++] = (int)slot;
    }
    const auto dir = own_logs_dir();
    if (dir.empty()) return;
    for (int si = 0; si < nslots; si++)
    {
        const auto path = dir / ("abptext_slot" + std::to_string(slots[si]) + ".txt");
        FILE *f = _wfopen(path.wstring().c_str(), L"wb");
        if (!f) continue;
        for (uint16_t r = 0; r < abp->num_rows; r++)
        {
            const auto &ri = abp->rows[r];
            if (ri.param_end_offset <= ri.param_offset && r + 1 >= abp->num_rows) continue;
            int32_t textId;
            std::memcpy(&textId, base + ri.param_offset + kTextOff, 4);
            const std::string s = goblin::raw_message_utf8((uint32_t)slots[si], (uint32_t)textId);
            std::fprintf(f, "%llu\t%d\t%s\n", (unsigned long long)ri.row_id, textId, s.c_str());
        }
        std::fclose(f);
        spdlog::info("[ABPTEXT] dumped ABP texts via slot {} -> {}", slots[si], path.string());
    }
}

void emevd_needle_scan_deferred()
{
    // Discovery (CreateFileW hook) fills disk_loot_dir asynchronously — poll for it.
    std::filesystem::path dir;
    for (int waited = 0; waited < 120000; waited += 500)
    {
        dir = emevd_dir();
        if (!dir.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (dir.empty())
    {
        spdlog::warn("[EMEVDSCAN] event dir never resolved — scan skipped");
        return;
    }
    spdlog::info("[EMEVDSCAN] scanning {} for arg needles 6250/5010...", dir.string());
    int hit_lines = 0, files = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        const auto &p = it->path();
        if (p.extension() != ".dcx" && p.extension() != ".emevd") continue;
        files++;
        emevd_scan_file(p, hit_lines);
    }
    spdlog::info("[EMEVDSCAN] done ({} files, {} hit lines{})", files, hit_lines,
                 hit_lines > 120 ? ", capped" : "");
    // GetMessage/msg-repo are resolved long before map discovery completes, so
    // this late point is safe for the raw-slot text probe.
    abp_text_probe();
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
        for (const char *dt : kDumpTables)
            if (name == dt)
                dump_table(name.c_str(), prc->param_header->param_table);
    }
    spdlog::info("[PARAMSCAN] done");

    // EMEVD leg: needs the async CreateFileW map-dir discovery → detached poller
    // (dev probe; same lifetime model as the other watcher threads).
    std::thread(&emevd_needle_scan_deferred).detach();
}
} // namespace goblin
