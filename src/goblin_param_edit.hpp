#pragma once
#include <cstdint>
#include <optional>

// ── Generic runtime param-FIELD editor (Slice 1 of the param-override loader) ─────────────────
//
// Writes a single field on a LIVE param row in the game's in-RAM param table. This is the atom of
// "mod Elden Ring WITHOUT shipping a regulation.bin": params are reloaded from regulation.bin on
// every boot, so an in-RAM field edit is SAVE-SAFE by construction — it evaporates on restart and
// the DLL re-applies it. Touches zero game files, composable with any overhaul.
//
// Plan + rationale: docs/plans/param_override_loader_plan.md.
// Precedents reused: from::params::get_param<T>/try_get (from/params.hpp), the WriteProcessMemory
// live-row write (goblin_kindling.cpp:safe_write_byte — clang-cl ELIDES __try around plain stores,
// so all cross-memory access must go through WPM/RPM, an opaque kernel call the optimizer can't drop).
//
// Slice 1 is OFFSET-addressed (caller passes the byte offset + width). Name-addressed editing
// (field name → offset via resolve_field_offset / runtime paramdef) is Slice 2 — do NOT expose a
// user-facing override file addressed by raw offset (silently wrong after a game patch).

namespace goblin::paramedit
{

enum class FieldType
{
    U8, S8, U16, S16, U32, S32, F32, F64, U64, S64,
};

// Byte width of a FieldType.
size_t field_width(FieldType t);

// Write `value` into `param[row_id]` at byte `offset`, narrowing to the field width. `value` is the
// numeric transport for every type; the writer converts. Returns false if the param table isn't
// loaded, the param/row is absent, `offset` is out of range for the row, or the write faults.
// NB U64/S64 magnitudes above 2^53 lose precision through `double` — add a raw-u64 overload if a mod
// ever needs full 64-bit exactness (none does today).
bool param_set_field(const wchar_t *param, uint64_t row_id, ptrdiff_t offset, FieldType type,
                     double value);

// Read `param[row_id]` at `offset` as `type`, returned as double. nullopt on the same failure modes.
// Used to VERIFY a write landed (read-back).
std::optional<double> param_get_field(const wchar_t *param, uint64_t row_id, ptrdiff_t offset,
                                      FieldType type);

}  // namespace goblin::paramedit
