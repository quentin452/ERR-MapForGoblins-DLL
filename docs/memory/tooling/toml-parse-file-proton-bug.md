---
name: toml-parse-file-proton-bug
description: toml::parse_file returns an EMPTY table under Proton/Wine — read the file yourself and toml::parse(string)
metadata:
  node_type: memory
  type: bugs
---

# `toml::parse_file` silently returns an empty table under Proton/Wine

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

**⚠️ Likely latent elsewhere:** `src/goblin_custom_items.cpp` still calls `toml::parse_file` for
`custom_items.toml`. If this bug is general (it appears to be), custom_items are **silently ignored under
Proton** — the apply loop just sees an empty table and applies nothing, no error. The author-items E2E
that "passed" may not have exercised a real on-disk toml under Wine. TODO: switch `custom_items.cpp` to
the ifstream+`toml::parse(string)` pattern and re-verify a real `custom_items.toml` applies under Proton.

**Note on toml++ exceptions mode:** in these DLL TUs toml++ builds with exceptions ON, so
`toml::parse_result` is an alias for `toml::table` and `parse`/`parse_file` THROW `toml::parse_error`
on malformed input (there is no `if(!result)` / `.error()` path — that only exists with
`TOML_EXCEPTIONS=0`). Catch the exception; don't test `operator bool`.
