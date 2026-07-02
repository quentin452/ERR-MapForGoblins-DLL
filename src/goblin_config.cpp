#include "goblin_config.hpp"
#include "goblin_config_schema.hpp"

#include <windows.h>

#include <spdlog/spdlog.h>
#include <mini/ini.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// NOTE: the goblin::config:: variables and the ini schema/emitter live in
// goblin_config_schema.cpp (kept dependency-free so tools/mfg_inigen links it).
// This file holds the runtime: ensure/migrate the ini, then load values.

namespace
{
    std::string to_lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Parse a string value into the entry's typed target variable.
    void set_from_string(const goblin::IniEntry &e, const std::string &v)
    {
        using goblin::IniType;
        switch (e.type)
        {
        case IniType::Bool:
            *static_cast<bool *>(e.target) = (v != "false");
            break;
        case IniType::U8:
        {
            int val;
            try { val = std::stoi(v); }
            catch (...) { val = 15; }
            if (val < 0 || val > 255) val = 15;
            *static_cast<uint8_t *>(e.target) = static_cast<uint8_t>(val);
            break;
        }
        case IniType::VkKey:
        {
            uint32_t vk = goblin::parse_vk_code(v);
            if (vk) *static_cast<uint32_t *>(e.target) = vk;
            break;
        }
        case IniType::GamepadMask:
        {
            uint16_t m = goblin::parse_gamepad_combo(v);
            if (m) *static_cast<uint16_t *>(e.target) = m;
            break;
        }
        case IniType::String:
            *static_cast<std::string *>(e.target) = v;
            break;
        case IniType::F32:
        {
            float f;
            try { f = std::stof(v); }
            catch (...) { f = 1.0f; }
            if (f < e.f32_min) f = e.f32_min;
            if (f > e.f32_max) f = e.f32_max;
            *static_cast<float *>(e.target) = f;
            break;
        }
        }
    }

    // Folder holding the ini (set before anything queries err_features_enabled) —
    // anchors the ERR-install fingerprint walk below.
    std::filesystem::path g_ini_folder;

    // Seed every config variable from its schema default. On a non-ERR install
    // ERR-only entries are force-disabled (so their features stay off even if
    // an ERR ini is present).
    void apply_defaults()
    {
        for (auto const &sec : goblin::ini_schema())
        {
            for (auto const &e : sec.entries)
            {
                bool err_only = e.err_only || sec.err_only;
                if (!goblin::err_features_enabled() && err_only)
                {
                    if (e.type == goblin::IniType::Bool)
                        *static_cast<bool *>(e.target) = false;
                    continue;
                }
                set_from_string(e, e.def);
            }
        }
    }
}

// Runtime ERR-install detection (replaces the compile-time MFG_VANILLA profile split).
// Fingerprint: ERR ships its menu-project dir in the mod overlay
// (menu/deploy/projects/ELDENRINGReforged). Probed with the same ancestor-walk as
// loot_disk's resolve_root_file — the DLL's own folder upward ("mod" overlay first,
// then the dir itself), then the eldenring.exe dir (UXM-style installs). Cached; the
// walk touches a handful of stat() calls once at init.
bool goblin::err_features_enabled()
{
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;
    namespace fs = std::filesystem;
    const fs::path rel = fs::path("menu") / "deploy" / "projects" / "ELDENRINGReforged";
    std::error_code ec;
    auto probe = [&](const fs::path &root) {
        return !root.empty() &&
               (fs::exists(root / "mod" / rel, ec) || fs::exists(root / rel, ec));
    };
    bool found = false;
    fs::path p = g_ini_folder;
    for (int up = 0; up < 5 && !found && !p.empty() && p != p.root_path();
         ++up, p = p.parent_path())
        found = probe(p);
    if (!found)
    {
        wchar_t buf[MAX_PATH] = {0};
        HMODULE h = GetModuleHandleW(L"eldenring.exe");
        if (h && GetModuleFileNameW(h, buf, MAX_PATH))
            found = probe(fs::path(buf).parent_path());
    }
    cached = found ? 1 : 0;
    spdlog::info("[PROFILE] ERR install {} (fingerprint {}) — ERR-only config {}",
                 found ? "DETECTED" : "not detected", rel.string(),
                 found ? "active" : "force-disabled");
    return found;
}

void goblin::ensure_ini(const std::filesystem::path &ini_path)
{
    namespace fs = std::filesystem;
    g_ini_folder = ini_path.parent_path();
    const bool include_err = err_features_enabled();

    mINI::INIStructure existing;
    bool had = fs::exists(ini_path);
    if (had)
    {
        mINI::INIFile f(ini_path.string());
        if (!f.read(existing)) had = false;
    }

    std::set<std::pair<std::string, std::string>> consumed; // lowercased (section,key)

    IniValueResolver resolve =
        [&](const std::string &section, const IniEntry &e, std::string &out) -> bool
    {
        if (!had) return false;
        std::string lsec = to_lower(section);
        if (existing.has(section) && existing[section].has(e.key))
        {
            out = existing[section].get(e.key);
            consumed.insert({lsec, to_lower(e.key)});
            return true;
        }
        if (e.rename_from && existing.has(section) && existing[section].has(e.rename_from))
        {
            out = existing[section].get(e.rename_from);
            consumed.insert({lsec, to_lower(e.rename_from)});
            return true;
        }
        return false;
    };

    std::ostringstream ss;
    emit_ini(ss, include_err, resolve);

    // Keys in the old file that the schema (for this build) didn't claim:
    // renamed-away, removed, or ERR-only in a vanilla build. Comment them out,
    // preserving the value, rather than silently dropping them.
    if (had)
    {
        std::vector<std::string> dead;
        for (auto const &sec_pair : existing)
        {
            const std::string &sname = sec_pair.first;
            for (auto const &kv : sec_pair.second)
            {
                if (!consumed.count({to_lower(sname), to_lower(kv.first)}))
                    dead.push_back("[" + sname + "] " + kv.first + " = " + kv.second);
            }
        }
        if (!dead.empty())
        {
            ss << "\n; ---------------------------------------------------------------\n"
               << "; No longer used by this version (renamed, removed, or not applicable\n"
               << "; to this build). Kept commented for reference; safe to delete.\n";
            for (auto const &d : dead) ss << "; " << d << "\n";
        }
    }

    std::string desired = ss.str();
    std::string current;
    if (had)
    {
        std::ifstream in(ini_path, std::ios::binary);
        std::ostringstream b;
        b << in.rdbuf();
        current = b.str();
    }
    if (!had || current != desired)
    {
        std::error_code ec;
        if (ini_path.has_parent_path()) fs::create_directories(ini_path.parent_path(), ec);
        std::ofstream out(ini_path, std::ios::binary | std::ios::trunc);
        out << desired;
        spdlog::info("Config: ini {}", had ? "re-synced" : "created with defaults");
    }
}

static std::filesystem::path g_loaded_ini_path;

const std::filesystem::path &goblin::config_ini_path()
{
    return g_loaded_ini_path;
}

void goblin::load_config(const std::filesystem::path &ini_path)
{
    spdlog::info("Config: {}", ini_path.string());
    g_loaded_ini_path = ini_path;

    ensure_ini(ini_path);
    apply_defaults();

    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    if (!file.read(ini))
    {
        spdlog::warn("Failed to read INI file, using defaults");
        return;
    }

    const bool include_err = err_features_enabled();
    for (auto const &sec : ini_schema())
    {
        if (sec.err_only && !include_err) continue;
        if (!ini.has(sec.name)) continue;
        auto &cfg = ini[sec.name];
        for (auto const &e : sec.entries)
        {
            if (e.err_only && !include_err) continue;
            std::string v;
            if (cfg.has(e.key))
                v = cfg.get(e.key);
            else if (e.rename_from && cfg.has(e.rename_from))
                v = cfg.get(e.rename_from);
            else
                continue;
            set_from_string(e, v);
            spdlog::debug("Config: {} = {}", e.key, v);
        }
    }
}

// Parse an XInput button combo string like "Y+R3" or "LB+RB+START" into a
// bitmask. Buttons separated by '+'. Returns 0 if any token is unknown.
uint16_t goblin::parse_gamepad_combo(std::string s)
{
    static const std::unordered_map<std::string, uint16_t> name_to_mask = {
        {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
        {"LB", 0x0100}, {"RB", 0x0200},
        {"L3", 0x0040}, {"LSTICK", 0x0040},
        {"R3", 0x0080}, {"RSTICK", 0x0080},
        {"BACK", 0x0020}, {"SELECT", 0x0020}, {"VIEW", 0x0020},
        {"START", 0x0010}, {"MENU", 0x0010},
        {"DPAD_UP", 0x0001}, {"UP", 0x0001},
        {"DPAD_DOWN", 0x0002}, {"DOWN", 0x0002},
        {"DPAD_LEFT", 0x0004}, {"LEFT", 0x0004},
        {"DPAD_RIGHT", 0x0008}, {"RIGHT", 0x0008},
    };
    uint16_t mask = 0;
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    size_t pos = 0;
    while (pos < s.size())
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '+'))
            pos++;
        size_t end = pos;
        while (end < s.size() && s[end] != '+' && s[end] != ' ' && s[end] != '\t')
            end++;
        if (end == pos) break;
        std::string tok = s.substr(pos, end - pos);
        auto it = name_to_mask.find(tok);
        if (it == name_to_mask.end())
        {
            spdlog::warn("Unknown gamepad button: '{}'", tok);
            return 0;
        }
        mask |= it->second;
        pos = end;
    }
    return mask;
}

// Reverse of parse_gamepad_combo: turn a bitmask back into a "Y+R3"-style string for ini save.
// One canonical name per bit (parse_gamepad_combo accepts aliases on read; write picks a single form).
std::string goblin::mask_to_combo_string(uint16_t mask)
{
    static const std::vector<std::pair<uint16_t, const char *>> canonical = {
        {0x1000, "A"}, {0x2000, "B"}, {0x4000, "X"}, {0x8000, "Y"},
        {0x0100, "LB"}, {0x0200, "RB"}, {0x0040, "L3"}, {0x0080, "R3"},
        {0x0020, "BACK"}, {0x0010, "START"},
        {0x0001, "UP"}, {0x0002, "DOWN"}, {0x0004, "LEFT"}, {0x0008, "RIGHT"},
    };
    std::string out;
    for (const auto &[bit, name] : canonical)
    {
        if (mask & bit)
        {
            if (!out.empty()) out += "+";
            out += name;
        }
    }
    return out;
}

// Parse a human key name ("F9", "A", "Home", "0", "Space") into Win32 VK_* code.
// Returns 0 on unknown name.
uint32_t goblin::parse_vk_code(std::string name)
{
    for (auto &c : name)
        if (c >= 'a' && c <= 'z') c -= 32; // uppercase

    if (name.empty()) return 0;

    if (name.size() >= 2 && name[0] == 'F')
    {
        try
        {
            int n = std::stoi(name.substr(1));
            if (n >= 1 && n <= 24) return 0x6F + n; // VK_F1=0x70
        }
        catch (...) {}
    }

    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
        return static_cast<uint32_t>(name[0]);
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
        return static_cast<uint32_t>(name[0]);

    static const std::pair<const char *, uint32_t> named[] = {
        {"SPACE", 0x20}, {"ESCAPE", 0x1B}, {"ESC", 0x1B},
        {"TAB", 0x09}, {"ENTER", 0x0D}, {"RETURN", 0x0D}, {"BACKSPACE", 0x08},
        {"HOME", 0x24}, {"END", 0x23},
        {"PAGEUP", 0x21}, {"PAGEDOWN", 0x22}, {"INSERT", 0x2D}, {"DELETE", 0x2E},
        {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27},
    };
    for (auto &p : named)
        if (name == p.first) return p.second;
    return 0;
}

void goblin::save_all_bool_settings(const std::filesystem::path &ini_path)
{
    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    file.read(ini);  // preserve comments / key order / non-bool entries

    // Write every Bool config var's CURRENT value back to its ini key (the menu
    // syncs runtime visibility into these vars before calling this). Non-bool
    // entries (keys, delays) are left as the file already has them.
    for (const auto &section : goblin::ini_schema())
    {
        if (section.err_only && !goblin::err_features_enabled())
            continue;
        for (const auto &e : section.entries)
        {
            if (e.target == nullptr)
                continue;
            if (e.err_only && !goblin::err_features_enabled())
                continue;
            // Bool/String/U8/F32/GamepadMask entries round-tripped from their config var so
            // menu-driven values (cluster_exclude, cluster_threshold, overlay_toggle_gamepad, …)
            // persist. VkKey untouched (no live keyboard rebind UI yet).
            if (e.type == goblin::IniType::Bool)
                ini[section.name][e.key] = *reinterpret_cast<bool *>(e.target) ? "true" : "false";
            else if (e.type == goblin::IniType::String)
                ini[section.name][e.key] = *reinterpret_cast<std::string *>(e.target);
            else if (e.type == goblin::IniType::U8)
                ini[section.name][e.key] = std::to_string(*reinterpret_cast<uint8_t *>(e.target));
            else if (e.type == goblin::IniType::F32)
                ini[section.name][e.key] = std::to_string(*reinterpret_cast<float *>(e.target));
            else if (e.type == goblin::IniType::GamepadMask)
                ini[section.name][e.key] = goblin::mask_to_combo_string(*reinterpret_cast<uint16_t *>(e.target));
        }
    }

    if (!file.write(ini))
        spdlog::warn("Config: failed to persist settings to {}", ini_path.string());
    else
        spdlog::info("Config: saved settings to {}", ini_path.string());
}

void goblin::reset_to_defaults_and_save(const std::filesystem::path &ini_path)
{
    // apply_defaults() (file-local, above) re-seeds every config var — including
    // questProgress="" and all visibility/cluster vars — from the schema. Then
    // persist: save_all_bool_settings reads the config vars (now defaults), so it
    // writes a defaults ini without going through the runtime-atomic sync that
    // persist_settings() does (which would otherwise clobber the reset).
    apply_defaults();
    save_all_bool_settings(ini_path);
    spdlog::info("Config: reset all settings to defaults in {}", ini_path.string());
}

void goblin::save_section_states(const std::filesystem::path &ini_path)
{
    mINI::INIFile file(ini_path.string());
    mINI::INIStructure ini;
    file.read(ini);  // load current file so write() preserves everything else

    auto &s = ini["Display Sections"];
    auto bstr = [](bool b) { return b ? "true" : "false"; };
    s["section_equipment"] = bstr(config::sectionEquipment);
    s["section_key_items"] = bstr(config::sectionKeyItems);
    s["section_loot"]      = bstr(config::sectionLoot);
    s["section_magic"]     = bstr(config::sectionMagic);
    s["section_quest"]     = bstr(config::sectionQuest);
    s["section_reforged"]  = bstr(config::sectionReforged);
    s["section_world"]     = bstr(config::sectionWorld);

    // write() updates values in place, preserving comments / key order / the
    // rest of the file. generate() would flatten it, so use write().
    if (!file.write(ini))
        spdlog::warn("Config: failed to persist section visibility to {}", ini_path.string());
}
