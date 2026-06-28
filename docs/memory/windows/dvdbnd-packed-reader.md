---
name: dvdbnd-packed-reader
description: "Strategy-C packed-install reader — parse ER's Data*.bhd/.bdt ourselves to read any game file (no internal game call). Format + keys + path-hash all empirically validated against the real packed install."
metadata: 
  node_type: memory
  type: reference
---

The no-crash-risk PACKED-install file source (<user> chose this over the virtual-FS internal-call RE,
2026-06-27): parse the encrypted dvdbnd archives ourselves, off-thread, pure data. Complements the
existing loose-file reader ([[category-icons-00solo-atlas]] read_game_file_decompressed): loose (mod
overlay/UXM) wins first; packed dvdbnd is the fallback. Feeds the SAME dcx_decompress + parsers.

**This box's install = genuinely PACKED** (<game_dir>): Data0-3.bhd/.bdt +
DLC.bhd/.bdt, no loose menu/ dir. eldenring.exe runs ERR via ME3 (mod files loose elsewhere). So the
item-icon sblytbnd is ONLY inside Data0 on this box — exactly the packed case to solve.

**FULLY VALIDATED in Python against the real files** (scratchpad/dvdbnd_probe.py, 2026-06-27):
- **.bhd = RSA-2048 encrypted header**, size is a multiple of 0x100. Decrypt per 256-byte cipher block:
  `m = c^e mod n` (public-key op), then **DROP the leading byte** → 255 plaintext bytes; concat → BHD5.
  (c < n always, since FromSoft "encrypted" with the private key.)
- **RSA pubkeys (PEM "RSA PUBLIC KEY", PKCS#1)** from BinderTool issue #44 (predates DLC → no DLC key
  there). Parsed (n,e): Data0 e=0x4731fc21, Data1 e=0x35aff625, **Data2 e=0xffffffff** (NOT 0x10001!),
  Data3 e=0x5eccde43; all moduli 256 bytes. ⚠️ exponents differ per archive — parse each, don't assume.
- **BHD5 header** (LE): "BHD5"(4) | sbyte bigEndianFlag(0x04) | bool Unk05(0x05) | 2 zero bytes |
  int32 version==1 (0x08) | int32 fileSize (0x0c) | bucketCount (0x10) | bucketsOffset | int32 saltLen
  | ASCII salt. is64Bit detect = peek int32@0x14 and @0x1c both 0 (ER = **is64Bit FALSE** → counts are
  int32). Salts: Data0 "GR_other", Data1 "GR_asset", Data2 "GR_map", Data3 "GR_chr".
- **Bucket** = int32 count, int32 filesOffset (is64 adds an int32 flag==1 — not for ER).
- **FileHeader (Game.EldenRing, 40 bytes)**: u64 FileNameHash | i32 PaddedFileSize | i32 UnpaddedFileSize
  (0 in practice) | i64 FileOffset | i64 SHAHashOffset | i64 AESKeyOffset. (totals: Data0 5824 / Data1
  39416 / Data2 39684 / Data3 1658 = 86582 entries.)
- **Path hash = 64-bit, prime 0x85** (133): lowercase, '\\'→'/', ensure leading '/', then
  `h = h*0x85 + ch` over u64. (NOT the DS1/DS3 32-bit prime-37.)
- **AESKey** (when AESKeyOffset!=0): 16-byte key + int32 rangeCount + Range[]{i64 start,i64 end}.
  Decrypt = **AES-128-ECB, no padding** (IV ignored), only the listed byte ranges in-place. Most asset
  files (incl. the sblytbnd) have aesOff=0 → no AES; handle the AES case for robustness (regulation etc.).
- **CONFIRMED target:** `/menu/hi/01_common.sblytbnd.dcx` → Data0, off=3544326800, padded=21056, aes=0.
  Slicing Data0.bdt at that off → valid **DCX (magic "DCX\0", DCP "KRAK", uncompressed 399279)** → our
  dcx_decompress(KRAK via oo2core) → BND4 → parse_item_icon_layout. RAW file is DCX-wrapped; return as-is.

**✅ C++ SHIPPED + OFFLINE-VALIDATED + IN-GAME-VALIDATED + COMMITTED 2026-06-27 (branch
feat/dvdbnd-packed-reader, commit d371afc = the 4 dvdbnd files, build clean; the redundant early-Oodle-hook
edits were git-restored/discarded per <user>; not pushed).** In-game test via `MFG_TEST_FORCE_PACKED=1` (skips loose, forces dvdbnd;
`TEST - Force Packed (dvdbnd).BAT` in ERR root) PASSED: log `[DVDBND] Data0: 5824 file entries` +
`'menu/hi/01_common.sblytbnd.dcx' -> Data0 (21056 bytes) off=3544326800` + `dvdbnd:... -> 399535 bytes
(KRAK)` + `[ITEMLAYOUT] 3070 rects` (VANILLA, vs 4844 ERR loose → proves it read Data0 not the override),
ZERO errors/crash, overlay kept ticking. Affordance MFG_TEST_FORCE_PACKED added alongside the
both-paths-off MFG_TEST_NO_GAMEFILE. src/worldmap/dvdbnd_reader.{hpp,cpp} `goblin::dvdbnd::read_packed_file(
game_dir, vpath)`: bakes (n,e) per archive as byte constants; **RSA via Windows BCrypt — BCRYPT_PAD_NONE
WORKS for the public-key raw modexp** (BCryptEncrypt on an imported BCRYPT_RSAPUBLIC_BLOB; NO bignum
fallback needed); per-archive decrypted-header cache (mutex); AES-128-ECB path for aesOff!=0 (untested,
not hit by assets). Wired as the fallback in read_game_file_decompressed (loot_disk.cpp): loose file
first (mod-aware) → packed dvdbnd(game_dir()) → DCX-decompress. CMake: added the 2 files + `bcrypt` link.
Offline test (scratchpad/dvdbnd_test, clang-cl + an spdlog no-op shim, compiles the REAL .cpp) returned
**21056 bytes, DCX-KRAK, first48 byte-for-byte == the Python ground truth → PASS.** DLL deployed to the
ERR offline dir. ⚠️ NOTE: branched off the in-progress early-Oodle-hook work (dllmain/goblin_inject
uncommitted edits ride along) — now arguably redundant with this reader; <user> to decide. NEXT = he
runtime-tests; then REUSE read_packed_file for GAP#2 DDS (00_Solo SB_Icon_* sheets are in the same dvdbnd
— hash their /menu/.../*.tpf path); DLC key (not in BinderTool#44) still needed to also scan DLC.bhd.
