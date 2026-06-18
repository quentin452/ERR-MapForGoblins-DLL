"""Tests for tools/analyze_events.py pure helpers.

Covered:
  - category()    : the thousands category digit (id//1000)%10
  - map_block()   : trailing-digit-drop grouping of sibling flags
  - is_map_instance() : the 10-digit map-instance range gate
  - decade()      : id-width bucket label
  - parse_events()/parse_data(): the regex log/data parsers, fed inline text
    via temp files (no real game logs).

All fixtures are inline; nothing reads the deployed MapForGoblins_events.log or
the committed goblin_map_data.cpp.
"""
import analyze_events as a


# --- category digit (id // 1000) % 10 -----------------------------------------

def test_category_thousands_digit():
    # 1042617188 -> ...7188 -> //1000 = 1042617 -> %10 = 7
    assert a.category(1042617188) == 7
    assert a.category(1000002000) == 2
    assert a.category(1000000999) == 0  # sub-thousand part ignored


# --- map-block grouping (collectible-shaped sibling grouping) -----------------

def test_map_block_drops_trailing_digits():
    assert a.map_block(1042617188, 4) == 104261  # drop last 4 digits
    assert a.map_block(1042617188, 6) == 1042     # coarser, whole-area
    # default block_drop is 4
    assert a.map_block(1042617188) == 104261


def test_map_block_siblings_share_prefix():
    # Two flags in the same room/area block collapse to one prefix.
    assert a.map_block(1042610001, 4) == a.map_block(1042619999, 4)
    # A flag in a different block does not.
    assert a.map_block(1042610001, 4) != a.map_block(1042620001, 4)


# --- map-instance range gate (collectible-shaped predicate) -------------------

def test_is_map_instance_range():
    assert a.is_map_instance(1_000_000_000) is True
    assert a.is_map_instance(1042617188) is True
    assert a.is_map_instance(999_999_999) is False
    assert a.is_map_instance(11109255) is False  # low/system/quest flag


# --- decade label -------------------------------------------------------------

def test_decade_label_by_width():
    assert a.decade(1042617188) == "10-digit"
    assert a.decade(12345) == "5-digit"


# --- regex parsers (fed inline temp files) ------------------------------------

def test_parse_events_splits_set_and_clear(tmp_path):
    log = tmp_path / "events.log"
    log.write_text(
        "some preamble\n"
        "flag 1042617188 = 1\n"
        "flag 1042617200 = 0\n"
        "flag 1042617188 = 1\n"   # duplicate set -> deduped by the set
        "noise line without a flag\n"
        "flag 99 = 1\n",
        encoding="utf-8",
    )
    set_ids, clear_ids, all_ids, n = a.parse_events(str(log))
    assert set_ids == {1042617188, 99}
    assert clear_ids == {1042617200}
    assert all_ids == {1042617188, 1042617200, 99}
    assert n == 4  # four matching "flag N = V" lines (noise/preamble excluded)


def test_parse_data_collects_flag_fields_and_skips_zero(tmp_path):
    cpp = tmp_path / "goblin_map_data.cpp"
    cpp.write_text(
        "{1ull, { .textDisableFlagId1 = 1042617188, .textDisableFlagId2 = 0 }, X},\n"
        "{2ull, { .clearedEventFlagId = 1042617200 }, X},\n"
        "{3ull, { .textId1 = 700001234 }, X},\n",  # not a flag field -> ignored
        encoding="utf-8",
    )
    known, per_field = a.parse_data(str(cpp))
    assert known == {1042617188, 1042617200}  # the 0 is skipped
    assert per_field["textDisableFlagId1"] == 1
    assert per_field["clearedEventFlagId"] == 1
    assert "textDisableFlagId2" not in per_field  # value 0 not counted
