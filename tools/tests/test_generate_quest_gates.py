"""Tests for tools/generate_quest_gates.py join/filter logic.

The high-value invariants of the quest-gate generator:
  (a) a name-collision false hit (e.g. "Rennala" matched by ranni's "Renna"
      keyword) is DROPPED when its nameId is not used by any marker;
  (b) only nameIds present in the WorldQuestNPC marker set are emitted;
  (c) the quest-active flags are carried through to each emitted row verbatim.

All fixtures are inline — no real npc_name_text_map.json, no generated
goblin_map_data.cpp. We test parse_worldquestnpc_nameids() and build_gate_rows()
directly (pure functions; file writing stays under __main__).
"""
import generate_quest_gates as g


# --- parse_worldquestnpc_nameids: pull marker nameIds out of baked cpp text ---

def test_parse_extracts_worldquestnpc_nameids_only():
    # Two WorldQuestNPC entries (textId1 = nameId + 700000000) and one of a
    # different Category that must be ignored.
    txt = """
    {123ull, { .textId1 = 700001234, .x = 1.0f }, Category::WorldQuestNPC, 0},
    {124ull, { .textId1 = 700005678, .y = 2.0f }, Category::WorldQuestNPC, 0},
    {125ull, { .textId1 = 700009999 }, Category::Grace, 0},
    """
    ids = g.parse_worldquestnpc_nameids(txt)
    assert ids == {1234, 5678}


def test_parse_skips_below_offset_textids():
    # textId1 < 700000000 is not a nameId-derived marker, must be skipped.
    txt = "{1ull, { .textId1 = 12345 }, Category::WorldQuestNPC, 0},"
    assert g.parse_worldquestnpc_nameids(txt) == set()


# --- build_gate_rows: the join / filter / flag-carry logic ---

# A broad "Renna" keyword name-collides: it substring-matches BOTH "Renna the
# Witch" (a real Ranni marker, nameId 200) and "Rennala" (nameId 999, NOT a
# marker). The marker-set filter is what protects against the false hit: 999 is
# not in MARKER_IDS so it is dropped. (The shipping script narrows the keyword to
# "Renna the Witch" as a second defence; here we test the filter itself.)
QUESTS = {
    "ranni":  (["Ranni the Witch", "Renna"], [1034509410, 1034509431]),
    "irina":  (["Irina"],                    [1045349207]),
}

NAME_MAP = {
    "200": "Renna the Witch",   # real Ranni marker nameId
    "999": "Rennala",           # collision on "Renna" -> must be dropped
    "300": "Irina",             # real Irina marker
    "400": "Irina",             # an Irina-named name with NO marker -> dropped
}

MARKER_IDS = {200, 300}  # nameIds a WorldQuestNPC marker actually uses


def test_collision_false_hit_is_dropped():
    rows, unmatched, _ = g.build_gate_rows(QUESTS, NAME_MAP, MARKER_IDS)
    emitted_ids = {nid for nid, _, _ in rows}
    # 999 ("Rennala") matched ranni's "Renna" substring but is not a marker.
    assert 999 not in emitted_ids
    assert "ranni" not in unmatched  # ranni still matched via the real 200


def test_only_marker_nameids_emitted():
    rows, _, _ = g.build_gate_rows(QUESTS, NAME_MAP, MARKER_IDS)
    emitted_ids = {nid for nid, _, _ in rows}
    assert emitted_ids == {200, 300}
    # 400 (Irina with no marker) is excluded even though the name matched.
    assert 400 not in emitted_ids


def test_dropped_counts_non_marker_hits():
    _, _, dropped = g.build_gate_rows(QUESTS, NAME_MAP, MARKER_IDS)
    # 999 (Rennala) and 400 (markerless Irina) are the two non-marker hits.
    assert dropped == 2


def test_flags_carried_through_verbatim():
    rows, _, _ = g.build_gate_rows(QUESTS, NAME_MAP, MARKER_IDS)
    by_id = {nid: (npc, flags) for nid, npc, flags in rows}
    assert by_id[200] == ("ranni", [1034509410, 1034509431])
    assert by_id[300] == ("irina", [1045349207])


def test_unmatched_npc_when_no_marker_hit():
    # A quest whose only name hit has no marker -> reported as unmatched.
    quests = {"ghost": (["Phantom"], [42])}
    name_map = {"500": "Phantom Knight"}
    rows, unmatched, dropped = g.build_gate_rows(quests, name_map, marker_ids=set())
    assert rows == []
    assert unmatched == ["ghost"]
    assert dropped == 1


def test_rows_sorted_by_nameid():
    quests = {"a": (["Zed"], [1]), "b": (["Abe"], [2])}
    name_map = {"900": "Zed", "100": "Abe"}
    rows, _, _ = g.build_gate_rows(quests, name_map, {900, 100})
    assert [nid for nid, _, _ in rows] == [100, 900]
