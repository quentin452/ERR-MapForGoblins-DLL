#!/usr/bin/env python3
"""Generate the quest-NPC visibility gate table (Thread 1 v1.5, quest-aware).

Joins two sources, NO reverse-engineering:
  1. Quest-active event flags per NPC, hand-extracted from the MIT-licensed
     EldenRingQuestLog (github.com/EldenRingQuestGiver/EldenRingQuestLog,
     src/erquestlog_quests.hpp): each `ADD_TALK_LIST_IF_DATA_ARGS(<npc>_quest,
     ..., <esd condition>)` line is the condition under which that NPC's quest is
     active/trackable. We flatten the condition to an OR-set of flag ids (the
     and/not nuance of a few NPCs — d/gurranq/hornsent — is approximated as OR;
     good enough for "is this questline live").
  2. nameId -> name from data/npc_name_text_map.json (the same names our markers
     resolve via textId1 = nameId + 700000000).

Output: src/generated/goblin_quest_gates.{hpp,cpp} — `QUEST_GATE[] = {nameId,
flags...}`. The DLL parks a WorldQuestNPC marker (areaNo 99) when its nameId is
gated AND none of its flags are set (quest not active), only when the opt-in
`quest_npc_quest_aware` config is on. Markers with no gate entry always show.

Run on Linux (pure stdlib). Re-run if the NPC table or name map changes.
"""
import json
import os
import re

HERE = os.path.dirname(__file__)
NAME_MAP = os.path.join(HERE, "..", "data", "npc_name_text_map.json")
MAP_DATA = os.path.join(HERE, "..", "src", "generated", "goblin_map_data.cpp")
OUT_HPP = os.path.join(HERE, "..", "src", "generated", "goblin_quest_gates.hpp")
OUT_CPP = os.path.join(HERE, "..", "src", "generated", "goblin_quest_gates.cpp")

# npc -> (name-match keywords [any substring hits], [quest-active flag ids]).
# Flags verbatim from EldenRingQuestLog quest-active conditions (OR-flattened).
QUESTS = {
    "irina":     (["Irina"],                       [1045349207]),
    "roderika":  (["Roderika"],                    [1041389406, 1041382735, 11109255]),
    "sellen":    (["Sellen"],                      [1044369227]),
    "kenneth":   (["Kenneth"],                     [1045389205]),
    "boc":       (["Boc"],                         [1043379355]),
    "blaidd":    (["Blaidd"],                      [1042369320, 1042369328]),
    "thops":     (["Thops"],                       [1039399215]),
    "patches":   (["Patches"],                     [31009206, 16009357]),
    "ranni":     (["Ranni the Witch", "Renna the Witch"], [1034509410, 1034509431]),
    "rya":       (["Rya"],                         [1037429209]),
    "gowry":     (["Gowry"],                       [1050389205]),
    "d":         (["D, Hunter", "D the Hunter"],   [1044399206, 1051439205]),
    "gurranq":   (["Gurranq"],                     [1051439205, 11109617]),
    "diallos":   (["Diallos"],                     [11109406]),
    "seluvis":   (["Seluvis"],                     [1034509312]),
    "dungeater": (["Dung Eater"],                  [11109955]),
    "rogier":    (["Rogier"],                      [10009617, 10009616, 10009619]),
    "nepheli":   (["Nepheli"],                     [10009706, 11109905]),
    "hyetta":    (["Hyetta"],                      [1039409205]),
    "alexander": (["Alexander"],                   [1043399306, 1051369255, 1051369265]),
    "yura":      (["Yura"],                        [1043379260]),
    "fia":       (["Fia"],                         [11109008]),
    "varre":     (["Varr"],                        [1042369206]),
    "millicent": (["Millicent"],                   [1050389258, 1038519256]),
    "jarbairn":  (["Jar-Bairn", "Jarbairn"],       [1039449255, 1039449256]),
    "corhyn":    (["Corhyn"],                      [11109855]),
    # Goldmask has no separate quest in EldenRingQuestLog — his steps live under
    # Corhyn's quest ("Searching for Goldmask", 8726xxxx), whose quest-active
    # condition is esd_get_flag(11109855). Gate his marker (111200) on the same
    # flag so it hides until the Corhyn/Goldmask questline is live.
    "goldmask":  (["Goldmask"],                    [11109855]),
    "latenna":   (["Latenna"],                     [1035429209]),
    "bernahl":   (["Bernahl"],                     [1042382713, 16009455, 16009456]),
    "ansbach":   (["Ansbach"],                     [2046429355, 2045429206]),
    "hornsent":  (["Hornsent"],                    [2048459278, 2046429210]),
    "queelign":  (["Queelign"],                    [21009212]),
    "ymir":      (["Ymir"],                        [2051459220]),
    "igon":      (["Igon"],                        [2048429208]),
    "trina":     (["Trina"],                       [22009255]),
}


def parse_worldquestnpc_nameids(txt):
    """Parse WorldQuestNPC marker nameIds (textId1 - 700000000) from baked
    goblin_map_data.cpp text. Pure (string in, set out) so it is unit-testable
    without the real generated file."""
    ids = set()
    # each MapEntry: {<id>ull, { ...data... }, Category::<C>, ...}
    for m in re.finditer(r"\{(.*?)\}, Category::(\w+),", txt, re.DOTALL):
        if m.group(2) != "WorldQuestNPC":
            continue
        t = re.search(r"\.textId1 = (\d+)", m.group(1))
        if t and int(t.group(1)) >= 700000000:
            ids.add(int(t.group(1)) - 700000000)
    return ids


def worldquestnpc_nameids():
    """nameIds actually used by WorldQuestNPC markers (textId1 - 700000000).
    Gating only these guarantees each gate is actionable AND drops name-collision
    false matches (e.g. Rennala for 'Renna', 'Sword of Bernahl', 'Fia's Champion')
    that aren't friendly-placed quest-NPC markers."""
    return parse_worldquestnpc_nameids(open(MAP_DATA, encoding="utf-8").read())


# nameIds to NEVER gate, even on a keyword hit — name-collision merchants that
# share a substring with a quest companion. The Hornsent Grandam (Bonny Village
# merchant) is NOT the Hornsent companion; the "Hornsent" keyword matched both,
# so her marker was wrongly hidden until the companion's quest activated (audit
# 2026-06-18). No substring separates "Hornsent" from "Hornsent Grandam", hence
# an explicit nameId exclusion.
EXCLUDE_NAMEIDS = frozenset({140200})  # Hornsent Grandam


def build_gate_rows(quests, name_map, marker_ids, exclude=EXCLUDE_NAMEIDS):
    """Join quest table + nameId->name map + marker nameId set.

    Pure: returns (rows, unmatched, dropped) where rows is a sorted list of
    (nameId, npc, flags). A keyword hit is only emitted when its nameId is in
    marker_ids (drops name-collision false hits, e.g. 'Rennala' for 'Renna');
    otherwise it is counted in `dropped`. Flags are carried through verbatim."""
    rows = []  # (nameId, npc, [flags])
    unmatched = []
    dropped = 0
    for npc, (keywords, flags) in quests.items():
        hit_ids = []
        for nid, name in name_map.items():
            if any(k.lower() in name.lower() for k in keywords):
                inid = int(nid)
                if inid in exclude:           # name-collision merchant, never gate
                    continue
                if inid in marker_ids:        # only gate nameIds a marker uses
                    hit_ids.append(inid)
                else:
                    dropped += 1
        if not hit_ids:
            unmatched.append(npc)
            continue
        for nid in hit_ids:
            rows.append((nid, npc, flags))
    rows.sort()
    return rows, unmatched, dropped


def main():
    name_map = json.load(open(NAME_MAP, encoding="utf-8"))
    marker_ids = worldquestnpc_nameids()
    rows, unmatched, dropped = build_gate_rows(QUESTS, name_map, marker_ids)
    print("WorldQuestNPC marker nameIds: %d; dropped %d non-marker name hits"
          % (len(marker_ids), dropped))

    with open(OUT_HPP, "w", encoding="utf-8") as f:
        f.write("#pragma once\n// AUTO-GENERATED by tools/generate_quest_gates.py\n")
        f.write("#include <cstddef>\n#include <cstdint>\n\n")
        f.write("namespace goblin::generated\n{\n\n")
        f.write("// A WorldQuestNPC marker whose nameId matches is shown only while\n")
        f.write("// ANY of its quest-active flags is set (quest-aware mode). Up to 4 flags.\n")
        f.write("struct QuestGate { uint32_t nameId; uint32_t flags[4]; };\n")
        f.write("extern const QuestGate QUEST_GATES[];\n")
        f.write("extern const size_t QUEST_GATE_COUNT;\n\n")
        f.write("} // namespace goblin::generated\n")

    with open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write('// AUTO-GENERATED by tools/generate_quest_gates.py\n')
        f.write('// Quest-active flags from EldenRingQuestLog (MIT); nameIds via npc_name_text_map.json.\n')
        f.write('#include "goblin_quest_gates.hpp"\n\nnamespace goblin::generated\n{\n\n')
        f.write("const QuestGate QUEST_GATES[] = {\n")
        for nid, npc, flags in rows:
            fl = (flags + [0, 0, 0, 0])[:4]
            f.write("    {%du, {%s}}, // %s\n" % (nid, ", ".join("%du" % x for x in fl), npc))
        f.write("};\n")
        f.write("const size_t QUEST_GATE_COUNT = %d;\n\n" % len(rows))
        f.write("} // namespace goblin::generated\n")

    print("wrote %d gate rows (%d NPCs matched)" % (len(rows), len(QUESTS) - len(unmatched)))
    if unmatched:
        print("UNMATCHED (no nameId — fix keywords):", ", ".join(unmatched))
    # show multi-nameId NPCs (states) for review
    from collections import Counter
    c = Counter(npc for _, npc, _ in rows)
    multi = {n: k for n, k in c.items() if k > 1}
    if multi:
        print("multi-nameId NPCs:", multi)


if __name__ == "__main__":
    main()
