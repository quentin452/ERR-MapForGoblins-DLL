# collision_offline — offline ER dvdbnd → collision reader (Linux)

Reads ER map collision straight out of the **packed** install (`Data0-3.bhd/.bdt`) with our own
RSA (SoulsFormats' `BHD5.Read` wants a decrypted header), the prime-0x85 path hash, and a `.bdt`
slice; then `SoulsFormats.BXF4` opens the `hkxbhd`/`hkxbdt` and lists the inner `hkx.dcx`.

```
dotnet run -- selftest                      # RSA/hash/slice self-check (Data0 known file + entry counts)
dotnet run -- list-collision m10_00_00_00   # inner hi-collision files of one map tile
dotnet run -- extract <vpath> <out.bin>     # raw (still-DCX) bytes of any dvdbnd file
```

`GAME_DIR` env overrides the game path. Collision is in **Data2** (salt `GR_map`); vpath convention
is `map/mMM/mMM_XX_YY_ZZ/hMM_XX_YY_ZZ.hkx{bhd,bdt}` (h=hi, l=lo).

## Status / wall
Everything up to the **BXF listing works offline on Linux** (validated 2026-07-06). The inner
`h*_######.hkx.dcx` are **DCX-KRAK (Oodle)**; SoulsFormats decompresses KRAK by P/Invoking
`oo2core_6_win64.dll` (a Windows PE) which a native Linux `dotnet` can't load → the HKX geometry +
`FSNPCustomParamCompressedMeshShape` material decode is blocked HERE. See
`docs/re/far_water_surface_disk_re_findings.md` §8 for the three Oodle routes (ooz `.so` /
RPC-hybrid / Wine C++ extractor) and the pick.

RSA keys are the same ER dvdbnd public keys already in `src/worldmap/dvdbnd_reader.cpp` (regenerate
`Keys.cs` from there if they ever change).
