#pragma once
#include <cstdint>
#include <optional>
#include <vector>

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
// A param's REAL row ids (first `max`), and its true row count via `out_total`. Enumerating beats
// probing ids: a param numbered by map id looks empty to a 0..N sweep. Empty + total 0 = the param
// is not resident (or absent from this install).
std::vector<uint64_t> param_row_ids(const wchar_t *param, size_t max, size_t *out_total);

std::optional<double> param_get_field(const wchar_t *param, uint64_t row_id, ptrdiff_t offset,
                                      FieldType type);

// ── Slice 2: name-addressed field access ─────────────────────────────────────────────────────
// Resolve (param, field-NAME) → offset via the game's OWN access instruction (modutils::
// resolve_field_offset AOB scan), the codebase's blessed version-proof + mod-agnostic method — the
// exe has NO queryable paramdef (docs/re/windows_live_paramdef_offset_re_findings.md), so a shipped
// paramdef table would be build-fragile; reading the compiled displacement out of the live exe is
// self-correcting across ER patches AND regulation swaps (vanilla/ERR/Convergence). Only fields
// with a registered access AOB resolve; unknown field → false/nullopt (caller can fall back to the
// offset-addressed API). This is what a user-facing override file must use (never a raw offset).
bool param_set_field_by_name(const wchar_t *param, uint64_t row_id, const char *field, double value);
std::optional<double> param_get_field_by_name(const wchar_t *param, uint64_t row_id,
                                               const char *field);

// True if (param, field) is in the resolvable-field registry (for a "field known?" check / listing).
bool field_is_known(const wchar_t *param, const char *field);

// ── Gap B: add rows to a live param table ────────────────────────────────────────────────────
// Generalizes the proven TutorialParam expansion (goblin_tutorial_popup.cpp): HeapAllocs a bigger
// param file, merges the existing rows + `templates` (sorted by id), rebuilds the row locators AND
// the 16-aligned id→index wrapper array (load-bearing for id-looked-up tables — a merely-4-aligned
// wrapper crashes on save-load), then swaps the ResCap's file_ptr/size. Mod-agnostic; stride +
// type-string are read from the LIVE table. SAVE-SAFE in the same sense as field edits (the param
// table reloads from regulation each boot; a row the SAVE references by id is the Gap-C concern, not
// this primitive). Row growth is copy-based (proven); this is the "mass-add" the audit flagged.
struct RowTemplate
{
    int32_t row_id;       // must be a FREE id — collision aborts the whole add (Gap H: no overwrite)
    const uint8_t *data;  // row_stride bytes of row data (e.g. cloned from an existing row + patched)
};

// Add `templates` rows to `param`. `row_stride` = 0 derives it from the table (rows[1]-rows[0]).
// Returns false if: param absent / <2 rows (can't derive stride) / any template id already exists
// (Gap H collision — logged [RESERVE]) / alloc fails. All-or-nothing.
bool param_add_rows(const wchar_t *param, const std::vector<RowTemplate> &templates,
                    int64_t row_stride = 0);

// Convenience: clone existing row `src_id` into a new row `new_id` (same data). Reads src's row data
// from the live table, then param_add_rows one template. Basis for custom items later (clone a
// template item, then param_set_field the clone). Returns false on the same conditions + src missing.
bool param_clone_row(const wchar_t *param, uint64_t src_id, int32_t new_id);

}  // namespace goblin::paramedit
