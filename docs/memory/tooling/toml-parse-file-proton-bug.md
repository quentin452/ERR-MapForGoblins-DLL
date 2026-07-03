---
name: toml-parse-file-proton-bug
description: toml++ parse returns EMPTY under Proton in the exceptions-ON config (BOTH parse_file AND parse(string)); the ONLY working fix is #define TOML_EXCEPTIONS 0. world_bundle's parse(string) path is latently broken.
metadata:
  node_type: memory
  type: bugs
---

# `toml::parse_file` silently returns an empty table under Proton/Wine

**SCOPE (narrowed 2026-07-03 by testing):** this bites ONLY the **exceptions-ON** toml++ config (the
default when a TU does NOT `#define TOML_EXCEPTIONS 0`). A TU that sets `#define TOML_EXCEPTIONS 0`
before including toml++ parses an on-disk file with `parse_file` **correctly** under Proton —
**`goblin_custom_items.cpp` does exactly this and is VERIFIED WORKING** (`test_author_items.py` 1/1:
`applied 1 custom item(s)`, `goods_count n=1`). So custom_items is FINE; do NOT "fix" it. The bug was
observed only in `goblin_world_bundle.cpp`, which used the DEFAULT (exceptions-ON) config.

**★ CORRECTION (2026-07-04, virtual_worlds C3):** the exceptions-ON config is broken for **BOTH**
`parse_file` **AND** `toml::parse(std::string)` — `goblin_virtual_world.cpp` used the exact
"ifstream + parse(string)" recipe below and its cold-boot load returned an **EMPTY** table (0 worlds), no
throw. Switching that TU to `#define TOML_EXCEPTIONS 0` + `parse_file` fixed it (E2E: save boot-1 → cold
boot → boot-load restored the world + 5 markers). So **the ONLY reliable fix is `#define TOML_EXCEPTIONS 0`**
(the custom_items config); the parse(string) mitigation does NOT work.

**⚠ world_bundle is LATENTLY SUSPECT:** it uses exceptions-ON + parse(string) (the now-disproven fix), so
its LOAD-from-disk almost certainly returns empty under Proton too. `test_world_editor.py` 24/24 passed
only because it never exercises a real cold-boot reload (it saves + applies the IN-MEMORY bundle in one
session). Migrate `goblin_world_bundle.cpp` to `#define TOML_EXCEPTIONS 0` + `parse_file` (like
custom_items / virtual_world) before relying on its on-disk load; add a genuine save→reboot→load test.

Historical (pre-correction) note — the two once-believed fixes: (1) `std::ifstream` +
`toml::parse(std::string)` — **DISPROVEN, also returns empty in exceptions-ON**; (2) `#define
TOML_EXCEPTIONS 0` before the toml++ include (matches custom_items) — **the only one that works.**

**Symptom (2026-07-03, World Editor slice 7 world_bundle):** `toml::parse_file(path)` inside the DLL
running under Proton returned an **empty** `toml::table` (`root.size()==0`) for a file that exists and is
valid TOML — **no exception thrown**, so the `catch (toml::parse_error&)` never fires and the code
proceeds as if the file were empty. Verified: the file on disk parsed perfectly with Python `tomllib`,
and `std::ofstream` had just written it to that exact path; only toml++'s own file open came back empty.
The path resolution is not the issue — the same path string round-trips through `std::ofstream` (save)
fine; it's `parse_file`'s internal file open that fails silently under Wine.

**Fix:** never use `toml::parse_file` in DLL code. Read the bytes yourself and parse the string:

```cpp
std::ifstream in(path, std::ios::binary);
if (!in) { /* log + bail */ }
std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
toml::table root = toml::parse(content);   // throws toml::parse_error (this TU builds exceptions ON)
```

`std::ifstream` (the mirror of the `std::ofstream` we already save with) resolves the path correctly
under Wine, and `toml::parse(std::string)` has no file-open step. This made `goblin_world_bundle`'s
save→load roundtrip go 0→2 ops (E2E `test_world_editor.py` 24/24).

**custom_items.cpp — CHECKED, NOT affected (2026-07-03).** It calls `toml::parse_file` too, but with
`#define TOML_EXCEPTIONS 0` at the top of the file, so it takes the exceptions-OFF parse path, which
loads the real on-disk `custom_items.toml` correctly under Proton (re-verified: `test_author_items.py`
1/1). No change needed. The "silently ignored" fear was config-wide over-generalization — the bug is
exceptions-ON-only.

**Note on toml++ exceptions mode:** in these DLL TUs toml++ builds with exceptions ON, so
`toml::parse_result` is an alias for `toml::table` and `parse`/`parse_file` THROW `toml::parse_error`
on malformed input (there is no `if(!result)` / `.error()` path — that only exists with
`TOML_EXCEPTIONS=0`). Catch the exception; don't test `operator bool`.
