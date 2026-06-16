#pragma once

#include <cstdint>

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
}
