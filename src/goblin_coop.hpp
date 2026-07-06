#pragma once
#include <vector>

#include "goblin_dll_export.hpp"  // GOBLIN_RENDER_API (markers() is read by the render map/minimap)

// Co-op player enumeration — mod-agnostic, ER-native (reads WorldChrMan's session player
// array, works for Seamless Co-op / vanilla summon / any co-op mod; no ersc.dll dependency).
//
// Primary use = gate the vmap world-freeze: `SetDisableAllChrUpdate` (goblin_pause) is a LOCAL
// sim freeze, so freezing while a seamless partner keeps simulating would desync the session.
// So the FREEZE_VMAP request skips the freeze when other players are present.
//
// RE: docs/re/coop_player_list_re_prompt.md ("SOLVED" — solo-validated). Player/session array =
// *(WorldChrMan + 0x10EF8) (ChrSet#0 @ WCM+0x10EE0, array field +0x18), capacity *(int*)(WCM+
// 0x10EF0) = 6 (local + up to 5 buddies). A slot is a real player iff its vtable == the LOCAL
// player's vtable (both CS::PlayerIns) — the RTTI filter with no hardcoded RVA.
namespace goblin::coop
{
    // Count of PlayerIns in the session array (local + buddies). 0 when unresolved / out of world.
    int player_count();

    // True iff >= 1 OTHER player is in the session (player_count > 1). Solo → false.
    bool others_present();

    // Pointers to the OTHER players' ChrIns (excludes the local player). Empty solo.
    std::vector<void *> other_players();

    // A co-op partner's projected map-space position (feeds the WorldMap + Minimap markers, drawn as the
    // native MENU_MAP_Host figure-in-ring — no rotation, since remote facing isn't reliably synced). `group`
    // = the map page (0 base-OW / 1 base-UG / 2 DLC-OW / 3 DLC-UG) so the marker only draws on the buddy's page.
    struct CoopMarker { float wx, wz; int group; };

    // Projected marker for each OTHER player. v1 uses the LOCAL player's tile (same-tile assumption — correct
    // when partners are together; a buddy in a different tile is misplaced until its MapId is RE'd). Empty solo.
    GOBLIN_RENDER_API std::vector<CoopMarker> markers();
}
