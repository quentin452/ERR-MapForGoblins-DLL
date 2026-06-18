# tools/tests — pytest suite for the Python generators

Fast, hermetic unit tests for the **pure logic** in `tools/*.py`. The C++ DLL is
game-hooked and can't be unit-tested here; these tests cover the offline
generator/analysis scripts that bake marker data on Linux.

No game data, no `regulation.bin`, no generated files are read — every test uses
tiny inline fixtures (and `tmp_path` for the regex parsers).

## Run

```sh
# from the repo root
python3 -m pytest tools/tests/ -q
```

If pytest is missing:

```sh
pip install --user pytest
```

`conftest.py` puts the parent `tools/` directory on `sys.path` so the scripts
import by module name (`import generate_quest_gates`, `import analyze_events`).

> Note: the suite lives under `tools/tests/` (not a top-level `tests/`) because
> only `tools/` and `src/` are writable in this environment. Run it with the
> path above, or add `tools/tests` to your own `pytest.ini` testpaths.

## What's covered

### `test_generate_quest_gates.py`
The quest-gate join/filter (`generate_quest_gates.build_gate_rows` and
`parse_worldquestnpc_nameids`):
- name-collision false hits are **dropped** (e.g. "Rennala" matched by ranni's
  "Renna" keyword, when its nameId isn't a real marker);
- only nameIds present in the WorldQuestNPC marker set are emitted;
- quest-active flags are carried through verbatim;
- `dropped`/`unmatched` bookkeeping and row sort order;
- `parse_worldquestnpc_nameids` ignores non-WorldQuestNPC categories and
  below-offset textIds.

### `test_analyze_events.py`
The event-analysis pure helpers:
- `category(id)` — the `(id//1000)%10` type digit;
- `map_block(id, drop)` — trailing-digit-drop sibling grouping;
- `is_map_instance(id)` — the `>= 1e9` collectible-shaped range gate;
- `decade(id)` — id-width bucket label;
- `parse_events` / `parse_data` — the log/cpp regex parsers (set vs clear split,
  zero-flag skipping, per-field counts), fed inline via `tmp_path`.
