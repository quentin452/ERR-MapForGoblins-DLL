#pragma once

#include <cstddef>
#include <cstdint>

namespace goblin::generated
{
    // English name-alias table for the F1 marker search: lets a player on a
    // non-English game type the English (wiki/MapGenie) name and still match a
    // marker whose live label is in the game's language. Keyed by the SAME
    // encoded id the markers carry as Marker::name_id, across every source the
    // search can encounter:
    //   items   — encode_live_item offsets (goods +500M, weapon/ammo +100M,
    //             protector +200M, accessory +300M, gem +400M), from items_database.json
    //   NPCs    — NpcName id + 700M (hostile/quest NPCs), from npc_name_text_map.json
    //   enemies — TutorialTitle/Codex id + 900M, from enemy_names_i18n.json (engus)
    //   bosses  — raw WorldMapPlaceName textId1, from boss_list.json (vanillaPlaceName)
    // Covers every PLACED/baked marker; anything absent falls back to
    // game-language-only matching (no regression). Strings are UTF-16LE.
    struct NameAliasEn
    {
        int32_t id;            // encoded name id (matches Marker::name_id)
        const wchar_t *name;   // English name
    };

    extern const size_t NAME_ALIAS_EN_COUNT;
    extern const NameAliasEn NAME_ALIASES_EN[];  // sorted ascending by id (binary-searchable)
}
