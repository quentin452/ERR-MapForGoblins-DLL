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

    /// English alias for an encoded marker name id (the bundled
    /// goblin_name_aliases_en table: items, NPCs, enemies, bosses), UTF-8. Empty
    /// if the id has no alias. Lets the F1 search match the English/wiki name on
    /// a non-English game (the live PlaceName label is in the game's language).
    /// Independent of the game's current language.
    std::string lookup_name_alias_en_utf8(int32_t id);
}
