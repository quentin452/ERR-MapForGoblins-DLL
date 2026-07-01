#pragma once

#include <cstdint>
#include <string>

namespace goblin
{
    /// Inject PlaceName text entries into MsgRepositoryImp at runtime.
    void setup_messages();

    /// Clear any injected-marker textId that has no string in the expanded
    /// PlaceName FMG (prevents a game-side null-wstring deref on load). Called
    /// at the end of setup_messages(), after the FMG bank is built.
    void sanitize_injected_textids();

    /// Toggle the PlaceName FMG slot between vanilla and expanded states.
    void set_fmg_injection_active(bool active);
    bool is_fmg_injection_active();

    /// Resolve a marker textId (item-name offset-encoded id, or raw PlaceName
    /// location id) to its string in the player's language, by reading the
    /// expanded PlaceName FMG we build at init. Returns nullptr if the id has
    /// no entry. Used by the marker dump to print exact loot/location names.
    const wchar_t *lookup_text(int32_t id);

    /// lookup_text + UTF-8 conversion (for ImGui, which wants UTF-8). Empty string if
    /// the id has no entry. Used by the overlay marker tooltips.
    std::string lookup_text_utf8(int32_t id);

    /// RAW native GetMessage by PHYSICAL fmg slot — RE probe surface only
    /// (goblin_param_scan [ABPTEXT]); normal lookups go through lookup_text_utf8.
    std::string raw_message_utf8(uint32_t fmg_slot, uint32_t msg_id);

    // The F1-search English alias (formerly lookup_name_alias_en_utf8, backed by
    // the baked goblin_name_aliases_en table) now resolves live from the active
    // install's engus FMGs on disk — see goblin::lookup_name_en_disk_utf8 in
    // worldmap/name_fmg_en.hpp.
}
