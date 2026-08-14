#pragma once

#include <cstdint>
#include <string>

// Runtime English-name resolution for the F1 marker search.
//
// The search wants ENGLISH (wiki/MapGenie) names so a player on a non-English
// game can still type the English name and match a marker. The live native
// GetMessage is language-locked to whatever the game loaded at startup, so we
// instead read the English FMGs straight off the active install's disk
// (msg/engus/*.msgbnd.dcx, via the dvdbnd/loose no-bake path) — automatically
// correct for whatever mod is loaded. This replaces the ERR-frozen baked
// goblin_name_aliases_en table (which shipped ERR's names into every profile).
namespace goblin
{
    // Read + index the English (engus) Name FMGs off disk. Idempotent; heavy
    // disk I/O + Oodle decompress — call ONCE, off the engine thread (dllmain
    // init). A no-op that leaves the index empty if the files can't be read
    // (search then degrades to game-language-only matching — no crash, no
    // wrong-mod names). May run once MORE via maybe_reload_english_index().
    void load_english_name_index();

    // English name for an ENCODED marker name_id (same offset scheme as
    // Marker::name_id / decode_textid: goods +500M, weapon +100M, protector
    // +200M, accessory +300M, gem +400M, npc +700M, place/boss raw). Returns
    // empty if the index isn't loaded or the id has no English entry. UTF-8.
    std::string lookup_name_en_disk_utf8(int32_t encoded_id);

    // Called by the CreateFileW observer when the GAME opens a .msgbnd: the init-time load
    // may have resolved the vanilla install (the capture is empty then; on a GA-like mount
    // the walk misses <root>/GA/). Flags a one-shot rebuild from the now-captured real files.
    void note_game_opened_msgbnd();

    // One-shot re-run of the index (called on a slow cadence from hk_present). No-op unless
    // a game msgbnd open was seen after the initial load.
    void maybe_reload_english_index();
}
