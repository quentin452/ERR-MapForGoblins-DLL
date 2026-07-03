#include "goblin_param_edit.hpp"

#include <cstring>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <vector>
#include <cwchar>
#include <mutex>

#include <spdlog/spdlog.h>

#include "from/params.hpp"
#include "modutils.hpp"        // resolve_field_offset — name→offset from the live exe
#include "re_signatures.hpp"   // field-access AOBs (goblin::sig::*)

namespace
{

// ── Name-addressed field registry (Slice 2) ──────────────────────────────────────────────────
// One row per (param, field) we expose by name. `aob`/`disp_pos`/`disp_size`/`consensus` feed
// modutils::resolve_field_offset (the game's own access instruction → the compiled displacement);
// `type` is the field's read/write WIDTH (independent of the instruction's disp encoding); `fallback`
// is the pinned offset used + logged only if the AOB ever fails to resolve. Extend by adding a row
// (author the AOB in Ghidra, same as goblin_item_classify). Seeded from the AOBs already proven in
// the codebase (goodsType/sortGroupId/AEG-lot/bonfire-textId1).
struct FieldSpec
{
    const wchar_t *param;
    const char *field;
    const char *aob;
    int disp_pos;
    int disp_size;
    bool consensus;
    goblin::paramedit::FieldType type;
    ptrdiff_t fallback;
};

using goblin::paramedit::FieldType;
constexpr FieldSpec kFields[] = {
    {L"EquipParamGoods", "goodsType", goblin::sig::GOODS_TYPE_ACCESS, 7, 1, false, FieldType::U8, 0x3e},
    {L"EquipParamGoods", "sortGroupId", goblin::sig::GOODS_SORT_GROUP_ACCESS, 3, 1, false, FieldType::U8, 0x72},
    {L"AssetEnvironmentGeometryParam", "pickUpItemLotParamId", goblin::sig::AEG_PICKUP_LOT_ACCESS, 2, 4, false, FieldType::S32, 0xb8},
    {L"BonfireWarpParam", "textId1", goblin::sig::BONFIRE_TEXTID1_ACCESS, 3, 1, true, FieldType::S32, 0x30},
};
constexpr size_t kFieldCount = std::size(kFields);

// Index of the registry row matching (param, field), or -1.
ptrdiff_t find_field(const wchar_t *param, const char *field)
{
    for (size_t i = 0; i < kFieldCount; i++)
        if (std::wcscmp(kFields[i].param, param) == 0 && std::strcmp(kFields[i].field, field) == 0)
            return static_cast<ptrdiff_t>(i);
    return -1;
}

// Resolve (and memoize) the offset for registry row `idx` from the live exe; fallback on AOB miss.
// resolve_field_offset scans the whole image, so this is once-per-field, not per-call.
ptrdiff_t resolve_registry_offset(size_t idx)
{
    static std::array<ptrdiff_t, kFieldCount> cache{};
    static std::array<std::once_flag, kFieldCount> once{};
    std::call_once(once[idx], [idx] {
        const auto &f = kFields[idx];
        auto r = modutils::resolve_field_offset(
            {.aob = f.aob, .disp_pos = f.disp_pos, .disp_size = f.disp_size, .consensus = f.consensus});
        cache[idx] = r ? *r : f.fallback;
        std::string pn = from::params::internal::wstring_to_string(std::wstring(f.param));
        if (r)
            spdlog::info("[PARAMEDIT] {}.{} = +0x{:x} (live from exe)", pn, f.field, cache[idx]);
        else
            spdlog::warn("[PARAMEDIT] {}.{} AOB unresolved — fallback +0x{:x}", pn, f.field, cache[idx]);
    });
    return cache[idx];
}

// Locate a live param row: fills `out_base` (row data base pointer) and `out_size` (a conservative
// upper bound on the row's byte length, from the ParamRowInfo span). Walks the param list directly
// — the same traversal get_param<T> does — so we can also recover the row EXTENT for a bounds check,
// which the ParamTableSequence container hides. Returns false if the list isn't loaded yet, the
// param name is absent, or the row id isn't present.
bool find_row(const wchar_t *param, uint64_t row_id, uint8_t **out_base, size_t *out_size)
{
    auto *param_list = *from::params::param_list_address;
    if (param_list == nullptr) return false;

    from::params::ParamTable *table = nullptr;
    const std::wstring_view want{param};
    constexpr int kEntries =
        sizeof(param_list->entries) / sizeof(param_list->entries[0]);
    for (int i = 0; i < kEntries; i++)
    {
        auto *rescap = param_list->entries[i].param_res_cap;
        if (rescap == nullptr) continue;
        std::wstring_view name = from::params::dlw_c_str(&rescap->param_name);
        if (name == want && rescap->param_header != nullptr)
        {
            table = rescap->param_header->param_table;
            break;
        }
    }
    if (table == nullptr) return false;

    // Binary search (rows are id-sorted), with an out-of-order linear fallback — mirrors
    // ParamTableSequence::try_get so behavior matches the read path exactly.
    auto emit = [&](from::params::ParamRowInfo *row) {
        *out_base = reinterpret_cast<uint8_t *>(table) + row->param_offset;
        *out_size = (row->param_end_offset > row->param_offset)
                        ? static_cast<size_t>(row->param_end_offset - row->param_offset)
                        : 0;  // 0 = unknown extent → caller skips the bound check
        return true;
    };

    ptrdiff_t lo = 0, hi = static_cast<ptrdiff_t>(table->num_rows) - 1;
    while (lo <= hi)
    {
        ptrdiff_t mid = (lo + hi) / 2;
        auto *row = &table->rows[mid];
        if (row->row_id < row_id) lo = mid + 1;
        else if (row->row_id > row_id) hi = mid - 1;
        else return emit(row);
    }
    for (int i = 0; i < table->num_rows; i++)
    {
        if (table->rows[i].row_id == row_id) return emit(&table->rows[i]);
    }
    return false;
}

// Opaque-kernel write — WriteProcessMemory can't be elided by clang-cl the way a plain store's
// __try guard is (see goblin_kindling.cpp:safe_write_byte). Faulting/unmapped target → false.
bool safe_write(void *addr, const void *src, size_t n)
{
    SIZE_T w = 0;
    return WriteProcessMemory(GetCurrentProcess(), addr, src, n, &w) && w == n;
}
bool safe_read(const void *addr, void *dst, size_t n)
{
    SIZE_T r = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, dst, n, &r) && r == n;
}

}  // namespace

namespace goblin::paramedit
{

size_t field_width(FieldType t)
{
    switch (t)
    {
    case FieldType::U8:
    case FieldType::S8: return 1;
    case FieldType::U16:
    case FieldType::S16: return 2;
    case FieldType::U32:
    case FieldType::S32:
    case FieldType::F32: return 4;
    case FieldType::U64:
    case FieldType::S64:
    case FieldType::F64: return 8;
    }
    return 0;
}

bool param_set_field(const wchar_t *param, uint64_t row_id, ptrdiff_t offset, FieldType type,
                     double value)
{
    if (offset < 0) return false;
    const size_t width = field_width(type);

    uint8_t *base = nullptr;
    size_t row_size = 0;
    try
    {
        if (!find_row(param, row_id, &base, &row_size)) return false;
    }
    catch (...)
    {
        return false;
    }
    // Bound the write to the row when the extent is known (0 = unknown → trust the caller's offset;
    // Slice 2's paramdef gives real bounds). Rejects wildly out-of-range offsets outright.
    if (row_size != 0 && static_cast<size_t>(offset) + width > row_size) return false;

    // Narrow `value` to the field's representation.
    uint8_t buf[8] = {0};
    switch (type)
    {
    case FieldType::U8:  { auto v = static_cast<uint8_t>(value);  std::memcpy(buf, &v, 1); break; }
    case FieldType::S8:  { auto v = static_cast<int8_t>(value);   std::memcpy(buf, &v, 1); break; }
    case FieldType::U16: { auto v = static_cast<uint16_t>(value); std::memcpy(buf, &v, 2); break; }
    case FieldType::S16: { auto v = static_cast<int16_t>(value);  std::memcpy(buf, &v, 2); break; }
    case FieldType::U32: { auto v = static_cast<uint32_t>(value); std::memcpy(buf, &v, 4); break; }
    case FieldType::S32: { auto v = static_cast<int32_t>(value);  std::memcpy(buf, &v, 4); break; }
    case FieldType::F32: { auto v = static_cast<float>(value);    std::memcpy(buf, &v, 4); break; }
    case FieldType::U64: { auto v = static_cast<uint64_t>(value); std::memcpy(buf, &v, 8); break; }
    case FieldType::S64: { auto v = static_cast<int64_t>(value);  std::memcpy(buf, &v, 8); break; }
    case FieldType::F64: { std::memcpy(buf, &value, 8); break; }
    }

    if (!safe_write(base + offset, buf, width))
    {
        spdlog::warn("[PARAMEDIT] write FAILED {}[{}] +0x{:x} w{}",
                     from::params::internal::wstring_to_string(std::wstring(param)), row_id,
                     offset, width);
        return false;
    }
    return true;
}

std::optional<double> param_get_field(const wchar_t *param, uint64_t row_id, ptrdiff_t offset,
                                      FieldType type)
{
    if (offset < 0) return std::nullopt;
    const size_t width = field_width(type);

    uint8_t *base = nullptr;
    size_t row_size = 0;
    try
    {
        if (!find_row(param, row_id, &base, &row_size)) return std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
    if (row_size != 0 && static_cast<size_t>(offset) + width > row_size) return std::nullopt;

    uint8_t buf[8] = {0};
    if (!safe_read(base + offset, buf, width)) return std::nullopt;

    switch (type)
    {
    case FieldType::U8:  { uint8_t  v; std::memcpy(&v, buf, 1); return static_cast<double>(v); }
    case FieldType::S8:  { int8_t   v; std::memcpy(&v, buf, 1); return static_cast<double>(v); }
    case FieldType::U16: { uint16_t v; std::memcpy(&v, buf, 2); return static_cast<double>(v); }
    case FieldType::S16: { int16_t  v; std::memcpy(&v, buf, 2); return static_cast<double>(v); }
    case FieldType::U32: { uint32_t v; std::memcpy(&v, buf, 4); return static_cast<double>(v); }
    case FieldType::S32: { int32_t  v; std::memcpy(&v, buf, 4); return static_cast<double>(v); }
    case FieldType::F32: { float    v; std::memcpy(&v, buf, 4); return static_cast<double>(v); }
    case FieldType::U64: { uint64_t v; std::memcpy(&v, buf, 8); return static_cast<double>(v); }
    case FieldType::S64: { int64_t  v; std::memcpy(&v, buf, 8); return static_cast<double>(v); }
    case FieldType::F64: { double   v; std::memcpy(&v, buf, 8); return v; }
    }
    return std::nullopt;
}

bool field_is_known(const wchar_t *param, const char *field)
{
    return find_field(param, field) >= 0;
}

bool param_set_field_by_name(const wchar_t *param, uint64_t row_id, const char *field, double value)
{
    ptrdiff_t idx = find_field(param, field);
    if (idx < 0) return false;
    return param_set_field(param, row_id, resolve_registry_offset(idx), kFields[idx].type, value);
}

std::optional<double> param_get_field_by_name(const wchar_t *param, uint64_t row_id,
                                              const char *field)
{
    ptrdiff_t idx = find_field(param, field);
    if (idx < 0) return std::nullopt;
    return param_get_field(param, row_id, resolve_registry_offset(idx), kFields[idx].type);
}

// ── Gap B: add rows to a live param table (generalizes goblin_tutorial_popup.cpp) ─────────────

// id→index binary-search entry the engine reads from the wrapper header (LookupTutorialParam).
struct WrapperRowLocator
{
    int32_t row;
    int32_t index;
};

static from::params::ParamResCap *find_res_cap(const wchar_t *name)
{
    auto param_list = *from::params::param_list_address;
    if (!param_list) return nullptr;
    for (int i = 0; i < 186; i++)
    {
        auto *prc = param_list->entries[i].param_res_cap;
        if (!prc) continue;
        std::wstring_view pn = from::params::dlw_c_str(&prc->param_name);
        if (pn == name) return prc;
    }
    return nullptr;
}

static std::string narrow(const wchar_t *w)
{
    return from::params::internal::wstring_to_string(std::wstring(w));
}

bool param_add_rows(const wchar_t *param, const std::vector<RowTemplate> &templates,
                    int64_t row_stride)
{
    if (templates.empty()) return true;

    from::params::ParamResCap *prc = find_res_cap(param);
    if (!prc || !prc->param_header)
    {
        spdlog::warn("[PARAMADD] {} not found", narrow(param));
        return false;
    }
    auto *rescap = reinterpret_cast<uint8_t *>(prc->param_header);
    auto *&file_ptr = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto &file_size = *reinterpret_cast<int64_t *>(rescap + 0x78);

    uint8_t *old_file = file_ptr;
    auto *old_table = reinterpret_cast<from::params::ParamTable *>(old_file);
    uint16_t orig_rows = old_table->num_rows;
    if (orig_rows < 2)
    {
        spdlog::warn("[PARAMADD] {} has {} rows — need >=2 to derive stride", narrow(param), orig_rows);
        return false;
    }
    int64_t stride = row_stride > 0
                         ? row_stride
                         : (int64_t)old_table->rows[1].param_offset -
                               (int64_t)old_table->rows[0].param_offset;
    if (stride <= 0 || stride > 0x100000)
    {
        spdlog::warn("[PARAMADD] {} bad stride {}", narrow(param), stride);
        return false;
    }

    // Gap H collision-check: a template id that already exists aborts the whole add (never overwrite).
    for (const auto &t : templates)
        for (uint16_t i = 0; i < orig_rows; i++)
            if ((int32_t)old_table->rows[i].row_id == t.row_id)
            {
                spdlog::warn("[RESERVE] param_add_rows {} id {} already exists — abort (no overwrite)",
                             narrow(param), t.row_id);
                return false;
            }

    constexpr size_t WRAPPER_HEADER = 0x10;
    constexpr size_t HEADER_SIZE = 0x40;
    const char *type_str = reinterpret_cast<const char *>(old_file + old_table->param_type_offset);
    size_t type_str_len = strlen(type_str) + 1;

    uint32_t total_rows = orig_rows + (uint32_t)templates.size();
    size_t row_locators_start = HEADER_SIZE;
    size_t data_start = row_locators_start + total_rows * sizeof(from::params::ParamRowInfo);
    size_t data_end = data_start + total_rows * (size_t)stride;
    size_t type_str_start = data_end;
    size_t after_type_str = type_str_start + type_str_len;
    // 16-align the wrapper array: the engine's id-lookup rounds this base UP to 16 before its binary
    // search, so a merely-4-aligned array reads past itself → OOB → save-load crash on id-looked-up
    // tables (goblin_tutorial_popup.cpp comment). Correct + harmless for iterated-only tables too.
    size_t wrapper_row_loc_start = (after_type_str + 0xf) & ~(size_t)0xf;
    size_t wrapper_row_loc_end = wrapper_row_loc_start + total_rows * sizeof(WrapperRowLocator);
    size_t param_file_size = wrapper_row_loc_end;
    size_t total_alloc = WRAPPER_HEADER + param_file_size;

    auto *allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total_alloc);
    if (!allocation)
    {
        spdlog::error("[PARAMADD] {} HeapAlloc {} bytes failed", narrow(param), total_alloc);
        return false;
    }
    auto *new_wrapper = reinterpret_cast<uint8_t *>(allocation);
    auto *new_file = new_wrapper + WRAPPER_HEADER;
    auto *new_table = reinterpret_cast<from::params::ParamTable *>(new_file);

    *reinterpret_cast<uint32_t *>(new_wrapper + 0x00) = (uint32_t)wrapper_row_loc_start;
    *reinterpret_cast<int32_t *>(new_wrapper + 0x04) = (int32_t)total_rows;

    memcpy(new_file, old_file, HEADER_SIZE);
    new_table->num_rows = (uint16_t)total_rows;
    new_table->param_type_offset = type_str_start;
    *reinterpret_cast<uint32_t *>(new_file + 0x00) = (uint32_t)type_str_start;
    *reinterpret_cast<uint64_t *>(new_file + 0x30) = data_start;
    memcpy(new_file + type_str_start, type_str, type_str_len);

    struct RowSrc { int32_t id; const uint8_t *data; };
    std::vector<RowSrc> all;
    all.reserve(total_rows);
    for (uint16_t i = 0; i < orig_rows; i++)
        all.push_back({(int32_t)old_table->rows[i].row_id, old_file + old_table->rows[i].param_offset});
    for (const auto &t : templates) all.push_back({t.row_id, t.data});
    std::sort(all.begin(), all.end(), [](const RowSrc &a, const RowSrc &b) { return a.id < b.id; });

    auto *new_loc = reinterpret_cast<from::params::ParamRowInfo *>(new_file + row_locators_start);
    auto *new_wloc = reinterpret_cast<WrapperRowLocator *>(new_file + wrapper_row_loc_start);
    size_t file_end_marker = type_str_start + type_str_len;
    for (size_t i = 0; i < all.size(); i++)
    {
        size_t off = data_start + i * (size_t)stride;
        new_loc[i].row_id = (uint64_t)all[i].id;
        new_loc[i].param_offset = off;
        new_loc[i].param_end_offset = file_end_marker;
        memcpy(new_file + off, all[i].data, (size_t)stride);
        new_wloc[i].row = all[i].id;
        new_wloc[i].index = (int32_t)i;
    }

    file_ptr = new_file;
    file_size = (int64_t)param_file_size;
    spdlog::info("[PARAMADD] {} expanded {} -> {} rows (+{}, stride {})", narrow(param), orig_rows,
                 total_rows, templates.size(), stride);
    return true;
}

bool param_clone_row(const wchar_t *param, uint64_t src_id, int32_t new_id)
{
    from::params::ParamResCap *prc = find_res_cap(param);
    if (!prc || !prc->param_header) return false;
    auto *rescap = reinterpret_cast<uint8_t *>(prc->param_header);
    uint8_t *file_ptr = *reinterpret_cast<uint8_t **>(rescap + 0x80);
    auto *table = reinterpret_cast<from::params::ParamTable *>(file_ptr);
    uint16_t rows = table->num_rows;
    if (rows < 2) return false;
    int64_t stride = (int64_t)table->rows[1].param_offset - (int64_t)table->rows[0].param_offset;
    if (stride <= 0 || stride > 0x100000) return false;

    const uint8_t *src = nullptr;
    for (uint16_t i = 0; i < rows; i++)
        if ((int32_t)table->rows[i].row_id == (int32_t)src_id)
        {
            src = file_ptr + table->rows[i].param_offset;
            break;
        }
    if (!src)
    {
        spdlog::warn("[PARAMADD] clone: {} src row {} not found", narrow(param), src_id);
        return false;
    }
    // Copy src into a local buffer that outlives the add (param_add_rows re-navigates + reallocs).
    std::vector<uint8_t> buf(src, src + stride);
    RowTemplate t{new_id, buf.data()};
    return param_add_rows(param, {t}, stride);
}

}  // namespace goblin::paramedit
