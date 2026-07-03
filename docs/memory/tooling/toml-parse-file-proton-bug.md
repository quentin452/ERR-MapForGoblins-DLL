---
name: toml-parse-file-proton-bug
description: toml::parse_file returns EMPTY under Proton ONLY in the exceptions-ON config; TOML_EXCEPTIONS=0 works. Fix = read file + toml::parse(string), or define TOML_EXCEPTIONS 0
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

**Two working fixes** (world_bundle uses the first): (1) read the file via `std::ifstream` +
`toml::parse(std::string)` (robust regardless of config); (2) `#define TOML_EXCEPTIONS 0` before the
toml++ include (matches custom_items). Prefer (1) — it doesn't depend on the exceptions macro.

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
