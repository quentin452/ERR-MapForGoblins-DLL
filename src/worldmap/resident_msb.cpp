// Resident-MSB loot source — see resident_msb.hpp for the design + RE pointers.
// The game keeps the DECOMPRESSED MSBs of the maps it has streamed (25 resident blobs
// measured live 2026-06-24), and they are BY CONSTRUCTION the active mod's maps (whatever
// loader mounted them — the engine read them, so they ARE what the game uses). Scanning
// them sidesteps the map-dir resolution entirely (the GA/ME3 miss: the walk only knew
// <p>/mod/, GA mounts <p>/GA/).
//
// ENUMERATION: bounded committed-private sweep for the "MSB " magic — the RE's OTHER proven
// route (the CSMapbndResCap vtable-instance walk was tried first and produced FALSE POSITIVES
// on 2.6.2.0: the vtable RVA matches as a VALUE inside the exe's RTTI/metadata tables, not as
// an object header, so no reliable name/bundle offsets — see the 2026-08-14 dump).
//
// NAME: the raw blob doesn't carry its own map id; the tile name lives in the FD4 resource
// key near the blob. We hunt the memory immediately BEFORE each blob for a UTF-16
// "m{AA}_{BB}_{CC}_{LOD}" string (the ResCap's name key / hash-string is allocated close to
// its bundle on the FD4 heap). Strict validation: m-pattern + _00 LOD only.

#include "resident_msb.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "msbe_parser.hpp"  // msbe::parse_msb

namespace
{
// SEH-guarded in-process read (same pattern as goblin_collected.cpp: no RPM, raw deref is
// free in-process; clang-cl preserves __try around a CALL so the fault lives in a noinline
// helper and the SEH wraps it).
__declspec(noinline) static void raw_copy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
static bool safe_read(const void *addr, void *out, size_t count)
{
    __try { raw_copy(out, addr, count); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <typename T> static bool rd(const void *addr, T &out)
{
    return safe_read(addr, &out, sizeof(T));
}

// Derive the decompressed-MSB blob length from the PARAM section chain (resident copy:
// ALL offsets — PARAM-level entryOffset[] AND nextParamOffset — are RELOCATED to absolute VAs
// (= blobBase + fileOffset), measured live 2026-08-14: entryOffset[0] = blob+0x80 etc. The old
// "PARAM-level stays file-absolute" assumption was WRONG for the resident copy. So the chain
// walk converts every offset by subtracting blobBase. 0 on malformed.
// `cap` = caller's region-bound sanity cap. `dbgBlob` = log the per-section walk for the first
// few blobs (layout RE when the chain still rejects everything).
static size_t resident_msb_len(const uint8_t *buf, size_t cap, uintptr_t blobBase, bool dbg = false)
{
    if (cap < 0x10 || std::memcmp(buf, "MSB ", 4) != 0) return 0;
    auto rd32 = [&](size_t o) -> uint32_t {
        uint32_t v = 0; safe_read(buf + o, &v, 4); return v;
    };
    auto rd64 = [&](size_t o) -> uint64_t {
        uint64_t v = 0; safe_read(buf + o, &v, 8); return v;
    };
    // header +0x08 = first PARAM offset: 0x10 as a FILE offset — the one field that stays
    // small in the resident copy too (the header isn't relocated).
    size_t po = rd32(0x08);
    if (po != 0x10 || po > cap) { if (dbg) spdlog::warn("[RESIDENTMSB]   len: bad first-po={} cap={}", po, cap); return 0; }
    size_t lastNext = 0;
    for (int s = 0; s < 6; s++)
    {
        if (po + 0x10 > cap) { if (dbg) spdlog::warn("[RESIDENTMSB]   len: sec{} po+0x10>cap po={} cap={}", s, po, cap); return 0; }
        uint32_t offsetCount = rd32(po + 4);
        if (offsetCount == 0 || offsetCount > 1000000u) { if (dbg) spdlog::warn("[RESIDENTMSB]   len: sec{} bad offsetCount={}", s, offsetCount); return 0; }
        uint32_t entries = offsetCount - 1;
        size_t entryArr = po + 0x10;
        if (entryArr + (size_t)entries * 8 + 8 > cap) { if (dbg) spdlog::warn("[RESIDENTMSB]   len: sec{} arr OOB entryArr={} entries={} cap={}", s, entryArr, entries, cap); return 0; }
        // nextParamOffset is an ABSOLUTE VA in the resident copy → convert to a file offset.
        uint64_t nxt = rd64(entryArr + (size_t)entries * 8);
        if (nxt < blobBase) { if (dbg) spdlog::warn("[RESIDENTMSB]   len: sec{} next 0x{:x} < blobBase 0x{:x} (NOT a VA?)", s, (unsigned long long)nxt, (unsigned long long)blobBase); return 0; }
        lastNext = (size_t)(nxt - blobBase);
        if (dbg) spdlog::warn("[RESIDENTMSB]   len: sec{} count={} entries={} next=0x{:x} -> fileOff=0x{:x}", s, offsetCount, entries, (unsigned long long)nxt, (unsigned long long)lastNext);
        po = lastNext;
    }
    // lastNext = nextParamOffset of the LAST section = end of file (validated: MSBs have no
    // trailing data past the last section's next offset).
    if (dbg) spdlog::warn("[RESIDENTMSB]   len: EOF=0x{:x}", (unsigned long long)lastNext);
    return (lastNext > 0x10 && lastNext <= cap) ? lastNext : 0;
}

// Raw MSB candidates WITHOUT the chain validation (for the dbg dump / auto-header-dump): the
// first kRawHits "MSB " magic addresses + their header bytes, so the real resident layout can
// be pinned when the section-chain walk rejects everything.
// ⚠ SAFETY: runs on the PRESENT thread via the RPC — a full committed-private sweep (~8 GB,
// 17 s) FROZE the game (2026-08-14). Bounded to kMaxBytes (1 GB ≈ ~2 s) — plenty to find the
// first resident MSBs (they stream in early) and dump their headers.
static std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> scan_msb_headers(size_t kRawHits = 8,
                                                                                size_t kMaxBytes = 1u << 30)
{
    std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> out;
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t aend = (uintptr_t)si.lpMaximumApplicationAddress;
    const size_t WIN = 8 * 1024 * 1024, OVL = 0x2200;
    std::vector<uint8_t> buf(WIN + OVL);
    std::unordered_map<uintptr_t, bool> seen;
    size_t scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < aend && out.size() < kRawHits && scanned < kMaxBytes &&
           VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        uintptr_t rbase = (uintptr_t)mbi.BaseAddress; size_t rsz = mbi.RegionSize;
        DWORD pr = mbi.Protect;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rsz >= 64 * 1024 &&
                  (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
                  !(pr & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok)
            for (size_t off = 0; off < rsz && out.size() < kRawHits && scanned < kMaxBytes;
                 off += WIN)
            {
                size_t want = rsz - off; if (want > WIN + OVL) want = WIN + OVL;
                if (scanned + want > kMaxBytes) want = kMaxBytes - scanned;
                if (!safe_read((void *)(rbase + off), buf.data(), want)) { scanned += want; continue; }
                scanned += want;
                size_t scan_to = (want > WIN) ? WIN : (want >= 4 ? want - 4 : 0);
                for (size_t i = 0; i < scan_to && out.size() < kRawHits; ++i)
                {
                    if (buf[i] != 'M' || buf[i + 1] != 'S' || buf[i + 2] != 'B' || buf[i + 3] != ' ')
                        continue;
                    uintptr_t blob = rbase + off + i;
                    if (seen.count(blob)) continue;
                    seen[blob] = true;
                    std::vector<uint8_t> hdr(buf.data() + i, buf.data() + i + 0x40);
                    out.emplace_back(blob, std::move(hdr));
                }
            }
        uintptr_t nxt = rbase + rsz; if (nxt <= addr) break; addr = nxt;
    }
    return out;
}

// Bounded committed-private sweep for the "MSB " magic. Each hit is header+chain-validated
// (resident_msb_len) — a false "MSB " inside another buffer fails the chain walk and is
// dropped. Dedup: the window overlap can re-hit the same blob; blob identity = its address.
// Early-exit cap: the RE measured ~25; `cap` is generous and bounds the worst case.
// ⚠ SAFETY: `kMaxBytes` bounds the sweep (the full ~8 GB walk took 17 s at 100% CPU and FROZE
// the game, 2026-08-14). The caller passes ~1 GB for the present-thread RPC dbg.
// NOTE: the RE'd live scan (2026-06-24, Altus) was IN-WORLD — no map is resident at the main
// menu, so a boot-time scan legitimately finds 0 until the player is in-game.
static void scan_msb_blobs(std::vector<goblin::worldmap::ResidentMsb> &out, size_t cap = 64,
                           size_t kMaxBytes = 1u << 30)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
    uintptr_t aend = (uintptr_t)si.lpMaximumApplicationAddress;
    const size_t WIN = 8 * 1024 * 1024, OVL = 0x2200;
    std::vector<uint8_t> buf(WIN + OVL);
    std::unordered_map<uintptr_t, bool> seen;
    size_t regions = 0, bytes = 0, raw_hits = 0, chain_fails = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < aend && out.size() < cap && bytes < kMaxBytes &&
           VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        uintptr_t rbase = (uintptr_t)mbi.BaseAddress; size_t rsz = mbi.RegionSize;
        DWORD pr = mbi.Protect;
        // MSB blobs are multi-MB FD4-heap buffers — skip tiny regions outright.
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rsz >= 64 * 1024 &&
                  (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
                  !(pr & (PAGE_GUARD | PAGE_NOACCESS));
        if (ok)
            for (size_t off = 0; off < rsz && out.size() < cap && bytes < kMaxBytes; off += WIN)
            {
                size_t want = rsz - off; if (want > WIN + OVL) want = WIN + OVL;
                if (bytes + want > kMaxBytes) want = kMaxBytes - bytes;
                if (!safe_read((void *)(rbase + off), buf.data(), want)) { bytes += want; continue; }
                bytes += want;
                size_t scan_to = (want > WIN) ? WIN : (want >= 4 ? want - 4 : 0);
                for (size_t i = 0; i < scan_to && out.size() < cap; ++i)
                {
                    if (buf[i] != 'M' || buf[i + 1] != 'S' || buf[i + 2] != 'B' || buf[i + 3] != ' ')
                        continue;
                    ++raw_hits;
                    uintptr_t blob = rbase + off + i;
                    if (seen.count(blob)) continue;
                    seen[blob] = true;
                    // Debug-walk the first few rejected blobs — pins WHERE the chain fails
                    // (relocation rule wrong? section walk assumption wrong?) in the log.
                    size_t len = resident_msb_len(buf.data() + i, want - i, blob,
                                                  /*dbg=*/raw_hits <= 3);
                    if (len == 0) { ++chain_fails; continue; }
                    goblin::worldmap::ResidentMsb m;
                    m.blob = blob;
                    m.len = len;
                    out.push_back(m);
                }
            }
        ++regions;
        uintptr_t nxt = rbase + rsz; if (nxt <= addr) break; addr = nxt;
    }
    spdlog::info("[RESIDENTMSB] sweep: {} regions, {} MB scanned, {} raw 'MSB ' hits, {} chain-valid "
                 "blobs ({} rejected by the section chain)", regions, (unsigned)(bytes >> 20),
                 raw_hits, out.size(), chain_fails);
    // Auto-dump the first raw candidates' headers when hits exist but NONE validate — the
    // resident layout clearly differs from the RE'd one (relocations on PARAM offsets? wrong
    // version/header fields?) and the header bytes pin it without needing an RPC round-trip.
    if (out.empty() && raw_hits > 0)
    {
        std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> hdrs = scan_msb_headers(8);
        spdlog::warn("[RESIDENTMSB] {} raw 'MSB ' hits, 0 valid — dumping first {} headers:",
                     raw_hits, (int)hdrs.size());
        for (size_t i = 0; i < hdrs.size(); ++i)
        {
            std::string hx;
            char b[8];
            for (size_t k = 0; k < hdrs[i].second.size(); ++k)
            {
                std::snprintf(b, sizeof(b), "%02x ", hdrs[i].second[k]);
                hx += b;
            }
            spdlog::warn("[RESIDENTMSB]   raw[{}] blob=0x{:x} hdr: {}", (int)i,
                         (unsigned long long)hdrs[i].first, hx);
        }
    }
}

// Hunt the tile name for a blob: UTF-16 "m{AA}_{BB}_{CC}_{LOD}" in the `lookback` bytes
// immediately BEFORE the blob (the FD4 resource key is allocated near its bundle). The
// LAST match before the blob wins (closest). Returns the name; empty if none found.
static std::string name_near_blob(uintptr_t blob, size_t lookback = 256 * 1024)
{
    const size_t WIN = 64 * 1024;
    std::vector<uint8_t> buf(WIN);
    std::string best;
    uintptr_t scanStart = blob > lookback ? blob - lookback : 0;
    for (uintptr_t p = blob; p > scanStart;)
    {
        size_t want = (size_t)(p - scanStart);
        if (want > WIN) want = WIN;
        p -= want;
        if (!safe_read((void *)p, buf.data(), want)) continue;
        // UTF-16: 'm\0' pairs. Scan backwards for the LAST candidate in this window.
        for (size_t i = want >= 2 ? want - 2 : 0; i + 2 <= want; --i)
        {
            if (i >= 2 && buf[i] == 'm' && buf[i + 1] == 0)
            {
                wchar_t wb[32]; size_t wn = 0;
                for (size_t j = i; j + 1 < want && wn < 31; j += 2, ++wn)
                {
                    wb[wn] = (wchar_t)(buf[j] | (buf[j + 1] << 8));
                    if (wb[wn] == 0) break;
                }
                std::string cand;
                for (size_t j = 0; j < wn && wb[j]; ++j)
                    cand += (char)wb[j];
                int a = 0, x = 0, z = 0, lod = -1;
                if (std::sscanf(cand.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) == 4 &&
                    a >= 0 && a <= 255 && x >= 0 && x <= 255 && z >= 0 && z <= 255 && lod == 0)
                {
                    best = cand;  // closest-so-far (walking backwards)
                    break;
                }
            }
            if (i == 0) break;
        }
        if (!best.empty()) return best;
    }
    return best;
}
} // namespace

namespace goblin::worldmap
{
// TTL cache + SAFETY: the FULL committed-private sweep (~8 GB, 17 s) froze the game at 100% CPU
// (freeze watchdog tripped, empty minidump, 2026-08-14). The scan is therefore BOUNDED to ~1 GB
// (~2 s) and cached with a TTL, so a bucket build pays it at most every kRescanTtlSec — and the
// resident set only grows as the player streams new maps (stale = under-reports, never wrong).
static constexpr int64_t kRescanTtlSec = 30;
static std::mutex g_scan_mtx;
static std::vector<ResidentMsb> g_scan_cache;
static int64_t g_scan_fail_t0 = 0;  // last scan that found ZERO blobs (backoff: don't hammer)

static int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<ResidentMsb> scan_resident_msbs()
{
    std::lock_guard<std::mutex> lk(g_scan_mtx);
    const int64_t t = now_ms();
    if (!g_scan_cache.empty() || (g_scan_fail_t0 && t - g_scan_fail_t0 < kRescanTtlSec * 1000))
    {
        if (!g_scan_cache.empty())
            return g_scan_cache;
        return {};
    }

    std::vector<ResidentMsb> out;
    scan_msb_blobs(out, 64, 1u << 30);   // bounded: ~2 s worst case, never the 17 s freeze
    // Name resolution: the blob itself doesn't carry its map id — hunt the UTF-16 tile name
    // in the memory just before it (FD4 key allocation). Strict: m-pattern + _00 only.
    std::vector<ResidentMsb> named;
    named.reserve(out.size());
    for (ResidentMsb &m : out)
    {
        std::string nm = name_near_blob(m.blob);
        if (nm.empty())
        {
            spdlog::debug("[RESIDENTMSB] blob=0x{:x} len={} — no tile name in the lookback window "
                          "(skipped)", (unsigned long long)m.blob, (unsigned long long)m.len);
            continue;
        }
        int a = 0, x = 0, z = 0, lod = -1;
        if (std::sscanf(nm.c_str(), "m%d_%d_%d_%d", &a, &x, &z, &lod) != 4 || lod != 0)
            continue;
        m.name = nm;
        m.area = (uint8_t)a; m.gx = (uint8_t)x; m.gz = (uint8_t)z;
        named.push_back(std::move(m));
    }
    spdlog::info("[RESIDENTMSB] {} MSB blobs scanned, {} named ({} unnamed — no tile name near "
                 "the blob)", (int)out.size(), (int)named.size(),
                 (int)out.size() - (int)named.size());
    for (const ResidentMsb &m : named)
        spdlog::debug("[RESIDENTMSB]   {} blob=0x{:x} len={}", m.name,
                      (unsigned long long)m.blob, (unsigned long long)m.len);

    g_scan_cache = named;
    if (named.empty()) g_scan_fail_t0 = t;
    return named;
}

std::string resident_msb_dbg()
{
    // Diagnostic: dump the magic-scanned blobs + the name-hunt result for each (the tile-name
    // lookback is the one heuristic in this module — this shows exactly what it found).
    // ⚠ PRESENT-THREAD SAFETY: bounded to ~1 GB scanned (~2 s) — the full sweep froze the game.
    std::string s;
    std::vector<ResidentMsb> blobs;
    scan_msb_blobs(blobs, 64, 1u << 30);
    char line[256];
    std::snprintf(line, sizeof(line), "resident_msb dbg: %d raw MSB blobs (in-world = maps are "
                  "streamed; main menu = 0 is EXPECTED)\n", (int)blobs.size());
    s = line;
    for (size_t i = 0; i < blobs.size() && i < 60; ++i)
    {
        std::string nm = name_near_blob(blobs[i].blob);
        std::snprintf(line, sizeof(line), "  [%2d] blob=0x%llx len=%llu name='%s'\n",
                      (int)i, (unsigned long long)blobs[i].blob,
                      (unsigned long long)blobs[i].len, nm.c_str());
        s += line;
    }
    // Raw header dump of the first unvalidated candidates — the chain walk rejects all of them
    // on some builds, and the header bytes show whether the resident layout differs (e.g.
    // relocations applied to the PARAM-level offsets) or the hits aren't MSBs at all.
    std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> hdrs = scan_msb_headers(8, 1u << 30);
    char hx[32];
    for (size_t i = 0; i < hdrs.size(); ++i)
    {
        std::snprintf(line, sizeof(line), "  raw[%d] blob=0x%llx hdr: ", (int)i,
                      (unsigned long long)hdrs[i].first);
        s += line;
        for (size_t k = 0; k < hdrs[i].second.size(); ++k)
        {
            std::snprintf(hx, sizeof(hx), "%02x ", hdrs[i].second[k]);
            s += hx;
        }
        s += "\n";
    }
    return s;
}

std::vector<DiskTreasure> load_resident_msbs(std::unordered_set<uint32_t> *coveredTiles,
                                             std::vector<DiskCollectible> *collectibles,
                                             std::vector<DiskEnemy> *enemies,
                                             std::vector<DiskRegion> *regions,
                                             std::vector<DiskObjAct> *objacts)
{
    std::vector<DiskTreasure> out;
    std::vector<ResidentMsb> blobs = scan_resident_msbs();
    if (blobs.empty()) return out;

    const bool wantAssets = collectibles != nullptr;
    const bool wantEnemies = enemies != nullptr;
    const bool wantRegions = regions != nullptr;
    const bool wantObjActs = objacts != nullptr;

    int parsed = 0;
    for (const ResidentMsb &m : blobs)
    {
        msbe::ParseResult r = msbe::parse_msb(reinterpret_cast<const uint8_t *>(m.blob), m.len,
                                              /*resident=*/true, /*blobBase=*/m.blob,
                                              wantAssets, wantEnemies, wantRegions,
                                              /*crossTileAssets=*/false, wantObjActs);
        if (!r.ok)
        {
            spdlog::warn("[RESIDENTMSB] parse failed: {} (len={})", m.name, m.len);
            continue;
        }
        ++parsed;
        if (coveredTiles)
            coveredTiles->insert(((uint32_t)m.area << 16) | ((uint32_t)m.gx << 8) | m.gz);

        // ── Same conversions as the disk loader (load_disk_treasures) — identical shapes ──
        if (wantAssets)
            for (const auto &a : r.assets)
            {
                DiskCollectible c;
                c.aegRow = a.aegRow;
                c.entityId = a.entityId;
                c.area = m.area; c.gx = m.gx; c.gz = m.gz;
                c.posX = a.pos[0]; c.posY = a.pos[1]; c.posZ = a.pos[2];
                c.name = a.name;
                c.modelName = a.modelName;
                collectibles->push_back(std::move(c));
            }
        if (wantEnemies)
            for (const auto &en : r.enemies)
            {
                DiskEnemy e;
                e.npcParamId = en.npcParamId;
                e.talkId = en.talkId;
                e.entityId = en.entityId;
                e.area = m.area; e.gx = m.gx; e.gz = m.gz;
                e.posX = en.pos[0]; e.posY = en.pos[1]; e.posZ = en.pos[2];
                e.name = en.name;
                e.modelName = en.modelName;
                enemies->push_back(std::move(e));
            }
        if (wantRegions)
            for (const auto &rg : r.regions)
            {
                DiskRegion d;
                d.subtype = rg.subtype;
                d.area = m.area; d.gx = m.gx; d.gz = m.gz;
                d.posX = rg.pos[0]; d.posY = rg.pos[1]; d.posZ = rg.pos[2];
                d.name = rg.name;
                regions->push_back(std::move(d));
            }
        if (wantObjActs)
            for (const auto &o : r.objacts)
            {
                if (o.partIndex < 0) continue;  // no placeable anchor (same rule as the disk pass)
                DiskObjAct d;
                d.objActParamId = o.objActParamId;
                d.entityId = o.objActEntityId ? o.objActEntityId : o.partEntityId;
                d.area = m.area; d.gx = m.gx; d.gz = m.gz;
                d.posX = o.pos[0]; d.posY = o.pos[1]; d.posZ = o.pos[2];
                d.partName = o.partName;
                objacts->push_back(std::move(d));
            }
        int tilePos = 0;
        for (const auto &t : r.treasures)
        {
            if (t.partIndex < 0) continue;  // item-glow / EMEVD-region → no MSB pos
            // Same inert-DummyAsset rule as the disk pass (only reachable dummies kept).
            if (t.partType == msbe::PART_DUMMY_ASSET && t.entityId == 0 && !t.entityGroup)
                continue;
            DiskTreasure d;
            d.lotId = t.itemLotId;
            d.area = m.area; d.gx = m.gx; d.gz = m.gz;
            d.posX = t.pos[0]; d.posY = t.pos[1]; d.posZ = t.pos[2];
            d.entityId = t.entityId;
            d.partName = t.partName;
            out.push_back(std::move(d));
            ++tilePos;
        }
        spdlog::debug("[RESIDENTMSB] {} -> {} treasures ({} positioned)", m.name,
                      r.treasures.size(), tilePos);
    }
    spdlog::info("[RESIDENTMSB] parsed {} resident MSBs -> {} treasures{}", parsed, out.size(),
                 coveredTiles ? (", " + std::to_string(coveredTiles->size()) + " tiles covered").c_str() : "");
    return out;
}
} // namespace goblin::worldmap
