// Overlay UI localization — see goblin_i18n.hpp for the design + file format.

#include "goblin_i18n.hpp"
#include "goblin_config.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

namespace goblin::i18n
{
namespace
{
    std::unordered_map<std::string, std::string> g_table;
    bool g_active = false;
    int g_generation = 0;
    std::filesystem::path g_mod_folder;  // remembered for the live switch

    // Unescape the file's `\n` / `\\` sequences into real characters.
    std::string unescape(const std::string &s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                const char c = s[i + 1];
                if (c == 'n') { out += '\n'; ++i; continue; }
                if (c == '\\') { out += '\\'; ++i; continue; }
            }
            out += s[i];
        }
        return out;
    }

    // "auto" → a language code from the OS user-interface language (Wine maps this from
    // the user's locale, so a French desktop reads as French under Proton too).
    std::string auto_language_code()
    {
        switch (PRIMARYLANGID(GetUserDefaultUILanguage()))
        {
        case LANG_FRENCH:     return "fr";
        case LANG_GERMAN:     return "de";
        case LANG_SPANISH:    return "es";
        case LANG_ITALIAN:    return "it";
        case LANG_PORTUGUESE: return "pt";
        case LANG_POLISH:     return "pl";
        case LANG_RUSSIAN:    return "ru";
        case LANG_JAPANESE:   return "ja";
        case LANG_KOREAN:     return "ko";
        case LANG_CHINESE:    return "zh";
        default:              return "en";
        }
    }

    // Load lang/<code>.txt into the table (replacing it). "" / "en" clears (English).
    // Returns false when a non-English table file is missing (state is then English).
    bool load_language(std::string code)
    {
        for (char &c : code) c = (char)tolower((unsigned char)c);
        if (code == "auto")
        {
            code = auto_language_code();
            // NB under Proton this reads the WINE prefix locale (often en_US regardless of
            // the desktop language) — set overlay_language explicitly if auto picks wrong.
            spdlog::info("[I18N] overlay_language auto -> '{}'", code);
        }
        g_table.clear();
        g_active = false;
        ++g_generation;
        if (code.empty() || code == "en")
        {
            spdlog::info("[I18N] overlay language: English (source strings, no table)");
            return true;
        }
        const std::filesystem::path file = g_mod_folder / "lang" / (code + ".txt");
        std::ifstream in(file);
        if (!in)
        {
            spdlog::warn("[I18N] overlay_language '{}' but {} not found — overlay stays English",
                         code, file.string());
            return false;
        }
        std::string line, pending_en;
        bool have_en = false;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("en=", 0) == 0)
            {
                pending_en = unescape(line.substr(3));
                have_en = !pending_en.empty();
            }
            else if (line.rfind("tr=", 0) == 0 && have_en)
            {
                std::string t = unescape(line.substr(3));
                if (!t.empty())
                    g_table.emplace(std::move(pending_en), std::move(t));
                have_en = false;
            }
        }
        g_active = !g_table.empty();
        spdlog::info("[I18N] overlay language '{}': {} strings loaded from {}", code,
                     g_table.size(), file.string());
        return true;
    }
} // namespace

const char *tr(const char *en)
{
    if (!g_active || !en || !en[0])
        return en;
    auto it = g_table.find(en);
    return it != g_table.end() ? it->second.c_str() : en;
}

bool active() { return g_active; }
int generation() { return g_generation; }

bool set_language(const char *code) { return load_language(code ? code : ""); }

std::vector<std::string> available_languages()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(g_mod_folder / "lang", ec))
    {
        if (!e.is_regular_file()) continue;
        const auto p = e.path();
        if (p.extension() == ".txt")
            out.push_back(p.stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void initialize(const std::filesystem::path &mod_folder)
{
    g_mod_folder = mod_folder;
    load_language(goblin::config::overlayLanguage);
}
} // namespace goblin::i18n
