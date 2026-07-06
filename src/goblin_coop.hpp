#pragma once
#include <vector>

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
    // Foundation for co-op map/minimap markers — the world-position projection per buddy is a
    // follow-up (needs each buddy's tile/MapId, validated on a live 2-player session).
    std::vector<void *> other_players();
}
