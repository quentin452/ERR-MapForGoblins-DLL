#pragma once
#include <filesystem>

namespace goblin::log_archive
{
    // Log lifecycle: call ONCE at DLL attach, BEFORE any logger opens its file.
    //
    // Bundles the PREVIOUS session's logs (every `MapForGoblins*.log` +
    // `MapForGoblins_flagcapture.txt` directly under `logs_dir`) into a single
    // timestamped archive `logs_dir/archive/MapForGoblins_<YYYYMMDD_HHMMSS>.zip`,
    // then deletes the originals so this session starts clean (no more sessions
    // piling into one file). Finally prunes `archive/` to the newest `keep_n` zips.
    //
    // Self-contained store-only (uncompressed) zip writer — zero dependencies. Never
    // throws; logging isn't up yet, so failures are silent (best-effort).
    void archive_and_rotate(const std::filesystem::path &logs_dir, int keep_n);
}
