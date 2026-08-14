# Offline data-parse tester (`tests/test_parse.cpp`)

Standalone binary that runs the **mod's own C++ parsers** (`msbe_parser.cpp` + the BND4/FMG
semantics of `name_fmg_en.cpp`) against real install files, so a parsing suspicion can be
proven/refuted **offline** — no game boot, no RPC, no DLL deploy. Born 2026-08-14 during the
Golden-Age Cartes hunt ("does the active mod's msgbnd carry the Map goods at all?" — the
hand-written Python BND4/FMG reader kept failing on the FMG v2 16-byte groups; using the mod's
own parser killed the question in one run).

## Build

Same toolchain as the DLL (clang-cl/xwin):

```
clang++ -std=c++17 -Isrc -Ithird_party tests/test_parse.cpp src/worldmap/msbe_parser.cpp ^
        src/stb_image_impl.cpp -o test_parse
```

## Usage

```
test_parse <mode> [options] <files...>
  modes:
    msgbnd  index every Name FMG in each msgbnd (BND4 -> FMG v2)
    msb     msbe::parse_msb each MSB, print section counts + sample treasures
    emevd   msbe::parse_emevd + emevd_inits, print award/init counts
    dcx     decompress each file and report sizes (sanity-checks oo2core)
  options:
    --cat <basename>   msgbnd only: index just this FMG (e.g. GoodsName.fmg)
    --filter <substr>  msgbnd only: print id=name rows containing <substr>
```

Examples (Golden Age install):

```
test_parse msgbnd --cat GoodsName.fmg --filter "map:" GA/msg/engus/item_dlc02.msgbnd.dcx
test_parse msb     GA/map/MapStudio/m60_46_38_00.msb.dcx
test_parse emevd   GA/event/common.emevd.dcx
test_parse dcx     <any .dcx>
```

## oo2core / GAME_DIR resolution

KRAK decompression needs the game's `oo2core_6_win64.dll`. The test resolves it with NO
hardcoded paths, in this order:

1. env `MFG_OO2CORE` (direct DLL path)
2. env `GAME_DIR` → `<GAME_DIR>/oo2core_6_win64.dll`
3. same keys parsed from the repo's `.env.local` (KEY=VALUE, `#` comments, optional `export`
   prefix, quote stripping — identical semantics to `tools/load_env.py`; searched in cwd, up to
   4 ancestor dirs, and the exe's own dir)
4. not found → KRAK files skip with a note (DFLT/zlib files still work)

`.env.local` is gitignored per-machine data — add `GAME_DIR=E:/SteamLibrary/steamapps/common/
ELDEN RING/Game` (or `MFG_OO2CORE=<path>`) there on any new machine; never hardcode install
paths in the test itself.

## Adding a new parse target

Each mode is ~30 lines over two shared helpers:

- `load_decompressed(path, oodle, &was_dcx, &was_krak)` — slurp + DCX (KRAK via oo2core);
- the `rd32/rd64/rdi32/utf16le_to_utf8` field readers (BND4/FMG).

Add a `mode_<x>` function mirroring `mode_msb` (parse with the mod's `msbe::` entry point,
print counts + a sample), hook it into the `main` dispatch, and extend this doc's mode list.
Keep the parser under test EXACTLY the shipped one (`msbe_parser.cpp`) — a hand-rolled parser
in the test is how this tool was born and is what it exists to prevent.
