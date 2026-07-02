#pragma once

// Single source of truth for the MapForGoblins.ini layout: every section,
// key, default value, comment, ERR-only flag and rename history lives here.
// Used to (a) GENERATE the shipped ini at build time (tools/mfg_inigen), and
// (b) by the DLL at runtime to create the ini if missing and to migrate an
// existing one (add new keys, apply renames, comment out obsolete keys).
//
// This translation unit must stay dependency-free (no spdlog/mINI) so the
// tiny inigen generator can link it without the rest of the mod.

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace goblin
{
    enum class IniType : uint8_t
    {
        Bool,        // -> bool*
        U8,          // -> uint8_t*
        VkKey,       // -> uint32_t*  (parsed via parse_vk_code)
        GamepadMask, // -> uint16_t*  (parsed via parse_gamepad_combo)
        String,      // -> std::string*  (raw value, stored verbatim)
        F32,         // -> float*  (parsed via std::stof)
    };

    struct IniEntry
    {
        const char *key;
        IniType type;
        void *target;            // address of the goblin::config:: variable
        const char *def;         // default, exactly as written in the ini
        const char *comment;     // may contain '\n' for multi-line; nullptr = none
        bool err_only;           // omitted + disabled in the vanilla build
        const char *rename_from; // previous key name in this section, or nullptr
        // IniType::F32 load-time clamp. Defaults match the original hardcoded [0.1, 5.0] this
        // replaced (dx-bugs 2026-07-01: that was a blanket range meant for the overlay *scale*
        // multipliers, e.g. overlay_master_scale, but IniType::F32 got reused later for fields
        // with very different valid ranges -- minimap_size (default 100px!) would silently get
        // clamped down to 5 on the very next load after any settings save, and minimap_zoom
        // could never persist a value someone had typed above 5.0 via Ctrl+click on the slider,
        // which ImGui does NOT clamp by default). Entries with a real range outside 0.1..5.0
        // MUST override these two.
        float f32_min = 0.1f;
        float f32_max = 5.0f;
    };

    struct IniSection
    {
        const char *name;
        const char *comment; // section header comment block (may be '\n'-split), or nullptr
        bool err_only;       // whole section omitted + disabled in vanilla
        std::vector<IniEntry> entries;
    };

    // The schema, built once.
    const std::vector<IniSection> &ini_schema();

    // Runtime replacement for the old compile-time MFG_VANILLA profile split: ONE DLL
    // for every install; ERR-only config sections/entries activate only when the active
    // install IS ELDEN RING Reforged (disk fingerprint, cached). Defined in
    // goblin_config.cpp (the runtime); mfg_inigen links only the schema and must not
    // call this — the shipped ini now always includes the ERR-only entries (they are
    // force-disabled at load time on a non-ERR install).
    bool err_features_enabled();

    // Resolver lets the caller supply an existing value for an entry (used by
    // the runtime migration to preserve user-set values + apply renames).
    // Return true and fill `value` to override the default; return false to
    // use the entry's default.
    using IniValueResolver =
        std::function<bool(const std::string &section, const IniEntry &e, std::string &value)>;

    // Emit a complete ini. ERR-only sections/entries are written only when
    // include_err_only is true. If `resolve` is set it supplies per-entry
    // values (else the schema default is used).
    void emit_ini(std::ostream &out, bool include_err_only, const IniValueResolver &resolve = {});
}
