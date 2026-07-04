---
name: toml-parse-file-proton-bug
description: toml++ parse returns EMPTY under Proton in the exceptions-ON config (BOTH parse_file AND parse(string)); the ONLY working fix is #define TOML_EXCEPTIONS 0. All DLL TOML configs (custom_items/virtual_world/world_bundle) now use it.
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

**✅ world_bundle MIGRATED + TESTED 2026-07-04.** `goblin_world_bundle.cpp` now `#define TOML_EXCEPTIONS 0`
+ `toml::parse_file` (was the disproven exceptions-ON `ifstream+parse(string)`). New
`tools/rpc_tests/test_world_bundle.py` is the genuine save→reboot→load: boot-1 records 1 clone + 1 set +
`bundle save`, cold boot, boot-2 `bundle status` reads back `clones=1 sets=1` FROM DISK (E2E 4/4 under
Proton). NB the bundle lives in `<ERR_ROOT>/dll/offline/` (the DLL's own folder), NOT `mod/`. So EVERY
DLL TOML config is now on the only-working `TOML_EXCEPTIONS 0` path.

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

**Fix (the ONLY one that works):** `#define TOML_EXCEPTIONS 0` before including toml++, then use
`toml::parse_file`:

```cpp
#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>
...
auto result = toml::parse_file(path.string());
if (!result) { spdlog::error("parse error: {}", std::string(result.error().description())); return false; }
const toml::table &root = result.table();
```

**DISPROVEN (do NOT use):** the `std::ifstream` + `toml::parse(std::string)` mitigation once believed to
work — under Proton it ALSO returns an empty table in the exceptions-ON config (virtual_worlds C3 +
world_bundle both confirmed this). The `test_world_editor.py` 24/24 that "proved" it never cold-boot-reloaded
(it saves+applies the in-memory bundle in one session), so it couldn't catch the empty on-disk load.

**custom_items.cpp — CHECKED, NOT affected (2026-07-03).** It calls `toml::parse_file` too, but with
`#define TOML_EXCEPTIONS 0` at the top of the file, so it takes the exceptions-OFF parse path, which
loads the real on-disk `custom_items.toml` correctly under Proton (re-verified: `test_author_items.py`
1/1). No change needed. The "silently ignored" fear was config-wide over-generalization — the bug is
exceptions-ON-only.

**Note on toml++ exceptions mode:** in these DLL TUs toml++ builds with exceptions ON, so
`toml::parse_result` is an alias for `toml::table` and `parse`/`parse_file` THROW `toml::parse_error`
on malformed input (there is no `if(!result)` / `.error()` path — that only exists with
`TOML_EXCEPTIONS=0`). Catch the exception; don't test `operator bool`.
