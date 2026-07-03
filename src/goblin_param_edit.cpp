#include "goblin_param_edit.hpp"

#include <cstring>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
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

}  // namespace goblin::paramedit
