#pragma once
// Packed-install file source — read a single game file straight out of Elden Ring's
// encrypted dvdbnd archives (Data0..Data3 .bhd/.bdt) that sit next to eldenring.exe.
// This is the PACKED-install fallback for read_game_file_decompressed: on a real
// packed game there are no loose .dcx files for us to slurp, but the archives are
// always on disk. We decrypt the RSA header ourselves, hash the virtual path, slice
// the file out of the .bdt — pure data + crypto, NO call into the game (so it can run
// off the engine thread with zero crash risk, unlike calling CSFile/DLFileDevice).
//
// Format + RSA keys + the 64-bit prime-0x85 path hash were validated against the real
// packed install before this was written (see [[dvdbnd-packed-reader]] / the scratchpad
// Python probe): /menu/hi/01_common.sblytbnd.dcx → Data0, a valid DCX-KRAK blob.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace goblin::dvdbnd
{
// Read one game file out of the packed Data*.bhd/.bdt dvdbnd in `game_dir` (the folder
// holding eldenring.exe + Data0.bhd…). `vpath` is the virtual asset path, e.g.
// "menu/hi/01_common.sblytbnd.dcx" (leading slash optional, case-insensitive — it is
// lowercased + '/'-normalised + prefixed internally). Returns the RAW stored bytes,
// still DCX-wrapped if the archive stored a .dcx — feed the result to msbe::dcx_decompress.
// Empty when: game_dir is empty / has no dvdbnd (loose/UXM install), the file isn't in any
// archive, or any decrypt/parse step fails. Does disk I/O + RSA on first touch of each
// archive (header cached after) — call off the engine thread / sparingly. Thread-safe.
std::vector<uint8_t> read_packed_file(const std::filesystem::path &game_dir,
                                      const std::string &vpath);
} // namespace goblin::dvdbnd
