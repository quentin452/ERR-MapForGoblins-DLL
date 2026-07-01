#include "msbe_parser.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#ifdef PARSER_COVERAGE
#include <memory>
#endif

#include "stb_image.h" // stbi_zlib_decode_buffer (raw zlib inflate, already in tree)

namespace goblin::msbe {

#ifdef PARSER_COVERAGE
namespace {
struct CoverageTracker {
    const uint8_t* buffer = nullptr;
    size_t size = 0;
    std::vector<uint8_t> accessed;

    CoverageTracker(const uint8_t* buf, size_t sz) : buffer(buf), size(sz), accessed(sz, 0) {}
    void mark(const uint8_t* p, size_t len) {
        if (p >= buffer && p + len <= buffer + size) {
            size_t offset = p - buffer;
            for (size_t i = 0; i < len; ++i) {
                accessed[offset + i] = 1;
            }
        }
    }
};
thread_local std::unique_ptr<CoverageTracker> g_tracker = nullptr;
} // namespace

void start_coverage(const uint8_t *buf, size_t len) {
    g_tracker = std::make_unique<CoverageTracker>(buf, len);
}

std::vector<uint8_t> get_coverage() {
    if (g_tracker) return g_tracker->accessed;
    return {};
}

void stop_coverage() {
    g_tracker.reset();
}
#endif

namespace {

// --- little-endian, bounds-checked readers over the decompressed blob -------------------
inline bool inb(size_t off, size_t need, size_t len) { return need <= len && off <= len - need; }

inline uint32_t rd32(const uint8_t *b, size_t o)
{
#ifdef PARSER_COVERAGE
    if (g_tracker) g_tracker->mark(b + o, 4);
#endif
    return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 |
           (uint32_t)b[o + 3] << 24;
}
inline uint64_t rd64(const uint8_t *b, size_t o)
{
    return (uint64_t)rd32(b, o) | ((uint64_t)rd32(b, o + 4) << 32);
}
inline float rdf(const uint8_t *b, size_t o)
{
    uint32_t v = rd32(b, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
// UTF-16LE -> ASCII (MSB part/name strings are ASCII-range "AEG099_..."); non-ASCII -> '?'.
std::string rd_utf16(const uint8_t *b, size_t o, size_t len)
{
    std::string s;
    size_t start = o;
    while (o + 1 < len)
    {
        uint16_t c = (uint16_t)b[o] | (uint16_t)b[o + 1] << 8;
        if (!c) {
            o += 2; // include the null-terminator in coverage
            break;
        }
        s.push_back(c < 0x80 ? (char)c : '?');
        o += 2;
        if (s.size() > 256) break;
    }
#ifdef PARSER_COVERAGE
    if (g_tracker) g_tracker->mark(b + start, o - start);
#endif
    return s;
}

constexpr uint32_t EVENT_TYPE_TREASURE = 4;
constexpr uint32_t EVENT_TYPE_OBJACT = 7; // ER MSB event subtype (Treasure=4, same numbering
                                          // family) — pinned empirically via [LOOTDISK] objact count
constexpr int SEC_MODEL = 0; // PARAM section order: MODEL,EVENT,POINT,ROUTE,LAYER,PARTS
constexpr int SEC_EVENT = 1; // PARAM section order: MODEL,EVENT,POINT,ROUTE,LAYER,PARTS
constexpr int SEC_POINT = 2; // POINT = Regions (the Spirit Springs source)
constexpr int SEC_PARTS = 5;
// MSBE region subtypes (@ region entry +0x08) — pinned tools/probe_region_layout.py.
constexpr int32_t REGION_MOUNT_JUMP = 46;
constexpr int32_t REGION_LOCKED_MOUNT_JUMP = 54;
constexpr int32_t REGION_OTHERS = -1;
// MSBE PartsParam part-type (@ PART entry +0x0c). 2 = Enemy (the enemy-drop source).
constexpr int32_t PART_ENEMY = 2;

// Parse "AEG{A}_{B}..." -> A*1000+B (= AssetEnvironmentGeometryParam row id).
// 0 if the name isn't an AEG asset. e.g. "AEG099_821_9000" -> 99821.
inline uint32_t aeg_row_from_name(const std::string &n, bool crossTile = false)
{
    // Model token "AEG{A}_{B}_…". Normally start-anchored. Cross-tile LOD proxies in a supertile
    // carry a "m{AA}_{BB}_{CC}_{LOD}-" tile prefix before the model (e.g.
    // "m60_48_57_00-AEG110_029_2000"); when crossTile, accept the model token after a "-AEG".
    size_t s = 0;
    if (n.compare(0, 3, "AEG") != 0)
    {
        if (!crossTile) return 0;
        size_t dash = n.find("-AEG");
        if (dash == std::string::npos) return 0;
        s = dash + 1;  // points at "AEG…"
    }
    if (n.size() < s + 8) return 0;
    size_t i = s + 3;
    uint32_t a = 0;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') { a = a * 10 + (n[i] - '0'); ++i; }
    if (i >= n.size() || n[i] != '_') return 0;
    ++i;
    uint32_t b = 0;
    bool any = false;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9') { b = b * 10 + (n[i] - '0'); ++i; any = true; }
    if (!any) return 0;
    return a * 1000 + b;
}

} // namespace

ParseResult parse_msb(const uint8_t *buf, size_t len, bool resident, uintptr_t blobBase,
                      bool wantAssets, bool wantEnemies, bool wantRegions, bool crossTileAssets,
                      bool wantObjActs)
{
    ParseResult R;
    if (len < 0x10 || std::memcmp(buf, "MSB ", 4) != 0) return R;

    // Resolve an ENTRY-INTERNAL offset (entry name / typeData): entry-relative on disk,
    // absolute VA in the resident copy. PARAM-LEVEL offsets (entryOffset[], next) are plain
    // file offsets in both -> index buf directly.
    auto eio = [&](uint64_t val, size_t entryStart) -> size_t {
        return resident ? (size_t)(val - blobBase) : entryStart + (size_t)val;
    };

    struct Sec { uint32_t entries; size_t entryArr; };
    Sec secs[6] = {};
    size_t po = rd32(buf, 0x08); // header +0x08 = first PARAM offset (= 0x10)
    for (int s = 0; s < 6; s++)
    {
        if (!inb(po, 0x10, len)) return R;
        uint32_t offsetCount = rd32(buf, po + 4); // = entries + 1
        if (offsetCount == 0 || offsetCount > 1000000u) return R;
        uint32_t entries = offsetCount - 1;
        size_t entryArr = po + 0x10; // long entryOffset[entries], then long nextParamOffset
        if (!inb(entryArr, (size_t)entries * 8 + 8, len)) return R;
        secs[s] = {entries, entryArr};
        po = (size_t)rd64(buf, entryArr + (size_t)entries * 8); // nextParamOffset (file-abs)
    }

    const Sec &EV = secs[SEC_EVENT];
    const Sec &PT = secs[SEC_PARTS];

    // MODEL section names (index -> name), only needed for the Asset modelIndex resolution.
    // Each MODEL entry stores its name at entry+0x00 (offset; entry-relative on disk, absolute
    // VA resident — same eio as parts). ERR substitutes some gather models (see Asset::modelName).
    std::vector<std::string> modelNames;
    if (wantAssets)
    {
        const Sec &MD = secs[SEC_MODEL];
        modelNames.reserve(MD.entries);
        for (uint32_t i = 0; i < MD.entries; i++)
        {
            size_t me = (size_t)rd64(buf, MD.entryArr + (size_t)i * 8);
            if (!inb(me, 0x08, len)) { modelNames.emplace_back(); continue; }
            size_t nm = eio(rd64(buf, me + 0x00), me);
            modelNames.push_back(nm < len ? rd_utf16(buf, nm, len) : std::string());
        }
    }

    for (uint32_t i = 0; i < EV.entries; i++)
    {
        size_t e = (size_t)rd64(buf, EV.entryArr + (size_t)i * 8);
        if (!inb(e, 0x28, len)) continue;
        if (rd32(buf, e + 0x0c) != EVENT_TYPE_TREASURE) continue;

        uint64_t tdOff = rd64(buf, e + 0x20);
        if (tdOff == 0) continue;
        size_t td = eio(tdOff, e);
        if (!inb(td, 0x14, len)) continue;

        Treasure t;
        t.itemLotId = rd32(buf, td + 0x10);
        if (t.itemLotId == 0 || t.itemLotId == 0xffffffffu) continue;

        uint32_t pidx = rd32(buf, td + 0x08);
        if (pidx != 0xffffffffu && pidx < PT.entries)
        {
            t.partIndex = (int32_t)pidx;
            size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)pidx * 8);
            if (inb(pe, 0x2c, len))
            {
                size_t nm = eio(rd64(buf, pe + 0x00), pe);
                if (nm < len) t.partName = rd_utf16(buf, nm, len);
                t.partType = (int32_t)rd32(buf, pe + 0x0c); // 13=Asset, 9=DummyAsset
                t.pos[0] = rdf(buf, pe + 0x20);
                t.pos[1] = rdf(buf, pe + 0x24);
                t.pos[2] = rdf(buf, pe + 0x28);
                // Entity sub-struct (ptr @ part+0x60, entry-relative on disk /
                // absolute VA resident): EntityID@+0x00, EntityGroupIDs[8]@+0x1c.
                // A type-9 DummyAsset with either set is reachable (kept by the
                // caller). Needs the +0x60..+0x68 u64 in bounds.
                if (inb(pe, 0x68, len))
                {
                    uint64_t entOff = rd64(buf, pe + 0x60);
                    if (entOff)
                    {
                        size_t ent = eio(entOff, pe);
                        if (inb(ent, 0x3c, len))
                        {
                            t.entityId = rd32(buf, ent + 0x00);
                            for (size_t k = 0x1c; k < 0x3c; k += 4)
                            {
                                uint32_t g = rd32(buf, ent + k);
                                if (g != 0 && g != 0xffffffffu) { t.entityGroup = true; break; }
                            }
                        }
                    }
                }
            }
        }
        R.treasures.push_back(std::move(t));
    }

    // ObjAct events (Elevator / lever-lift source, MapGenie Group 2). An MSB ObjAct EVENT (subtype 7)
    // binds an asset placement to an ObjActParam prompt. Its typeData (ptr @ event+0x20, same eio rule
    // as the treasure typeData) carries objActEntityId @+0x00, part INDEX @+0x04 (i32, -1 = none), and
    // objActParamId @+0x08. The part resolves exactly like the treasure path (name@+0x00, pos@+0x20,
    // entity sub-struct @+0x60). The caller filters objActParamId to the lever/lift ObjActParam rows
    // (live ActionButtonParam text "Pull/Push lever"). See linux_group2_prompt_binding_re_findings.md.
    if (wantObjActs)
    {
        for (uint32_t i = 0; i < EV.entries; i++)
        {
            size_t e = (size_t)rd64(buf, EV.entryArr + (size_t)i * 8);
            if (!inb(e, 0x28, len)) continue;
            if (rd32(buf, e + 0x0c) != EVENT_TYPE_OBJACT) continue;

            uint64_t tdOff = rd64(buf, e + 0x20);
            if (tdOff == 0) continue;
            size_t td = eio(tdOff, e);
            if (!inb(td, 0x0c, len)) continue;

            ObjActEv o;
            o.objActEntityId = rd32(buf, td + 0x00);
            int32_t pidx = (int32_t)rd32(buf, td + 0x04);
            o.objActParamId = rd32(buf, td + 0x08);
            if (pidx != -1 && (uint32_t)pidx < PT.entries)
            {
                o.partIndex = pidx;
                size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)pidx * 8);
                if (inb(pe, 0x2c, len))
                {
                    size_t nm = eio(rd64(buf, pe + 0x00), pe);
                    if (nm < len) o.partName = rd_utf16(buf, nm, len);
                    o.pos[0] = rdf(buf, pe + 0x20);
                    o.pos[1] = rdf(buf, pe + 0x24);
                    o.pos[2] = rdf(buf, pe + 0x28);
                    // Entity sub-struct (ptr @ part+0x60, EntityID@+0x00 — same read as the
                    // treasure/asset paths). Fallback anchor when objActEntityId is 0.
                    if (inb(pe, 0x68, len))
                    {
                        uint64_t entOff = rd64(buf, pe + 0x60);
                        if (entOff)
                        {
                            size_t ent = eio(entOff, pe);
                            if (inb(ent, 0x04, len))
                            {
                                uint32_t eid = rd32(buf, ent + 0x00);
                                if (eid != 0xffffffffu) o.partEntityId = eid;
                            }
                        }
                    }
                }
            }
            R.objacts.push_back(std::move(o));
        }
    }

    // Collectibles: enumerate every Asset part (type 13) named "AEG..." with its
    // block-local position. The caller resolves each aegRow ->
    // AssetEnvironmentGeometryParam.pickUpItemLotParamId -> ItemLotParam_map LIVE.
    if (wantAssets)
    {
        for (uint32_t i = 0; i < PT.entries; i++)
        {
            size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)i * 8);
            if (!inb(pe, 0x2c, len)) continue;
            if ((int32_t)rd32(buf, pe + 0x0c) != PART_ASSET) continue;
            // GameEditionDisable (int @ part+0x44 — inline, same offset disk/resident; pinned
            // vs SoulsFormats 6612/6612 incl. 30 positives, tools/probe_gameedition_offset.py):
            // a disabled placement the engine never spawns. The bake's generators skip these
            // (GameEditionDisable==1), so drop them too — else e.g. the m60_45_39 Imp seal draws
            // a phantom marker the player can't interact with.
            if (inb(pe, 0x48, len) && rd32(buf, pe + 0x44) == 1) continue;
            size_t nm = eio(rd64(buf, pe + 0x00), pe);
            if (nm >= len) continue;
            std::string name = rd_utf16(buf, nm, len);
            uint32_t row = aeg_row_from_name(name, crossTileAssets);
            if (row == 0) continue;
            Asset a;
            a.name = std::move(name);
            a.aegRow = row;
            // Actual model (modelIndex u32 @ part+0x14 -> MODEL names). Picks up ERR's model
            // substitution (vanilla part name, DLC model) so GEOF graying buckets by the real
            // model. The inb(pe,0x2c) check above already covers +0x14.
            {
                uint32_t mi = rd32(buf, pe + 0x14);
                if (mi < modelNames.size()) a.modelName = modelNames[mi];
            }
            a.pos[0] = rdf(buf, pe + 0x20);
            a.pos[1] = rdf(buf, pe + 0x24);
            a.pos[2] = rdf(buf, pe + 0x28);
            // EntityID (entity sub-struct ptr @ part+0x60, EntityID@+0x00 — identical to
            // the treasure/enemy read). Interactive World-feature assets (Imp seals,
            // Hero's Tomb statues) carry one; plain decoration does not. 0xffffffff = unset.
            if (inb(pe, 0x68, len))
            {
                uint64_t entOff = rd64(buf, pe + 0x60);
                if (entOff)
                {
                    size_t ent = eio(entOff, pe);
                    if (inb(ent, 0x04, len))
                    {
                        uint32_t eid = rd32(buf, ent + 0x00);
                        if (eid != 0xffffffffu) a.entityId = eid;
                    }
                }
            }
            R.assets.push_back(std::move(a));
        }
    }

    // Enemy drops: enumerate every Enemy part (type 2) with its NPCParamID + block-local
    // position. The caller resolves each npcParamId -> NpcParam.itemLotId_map/_enemy ->
    // ItemLotParam LIVE. NPCParamID lives in the Enemy typeData sub-struct: the u64 pointer
    // @ part+0x68 (entry-relative on disk / absolute VA resident, same eio() rule), then the
    // u32 @ +0x0c inside it. Pinned 26277/26277 vs SoulsFormats (probe_enemy_npc_offset.py).
    if (wantEnemies)
    {
        for (uint32_t i = 0; i < PT.entries; i++)
        {
            size_t pe = (size_t)rd64(buf, PT.entryArr + (size_t)i * 8);
            if (!inb(pe, 0x70, len)) continue;
            if ((int32_t)rd32(buf, pe + 0x0c) != PART_ENEMY) continue;
            // GameEditionDisable (int @ +0x44; same pin as assets) — drop disabled placements
            // the engine never spawns, so the no-bake enemy/hostile-NPC passes match the bake.
            if (inb(pe, 0x48, len) && rd32(buf, pe + 0x44) == 1) continue;
            uint32_t npc = 0;
            uint64_t tdOff = rd64(buf, pe + 0x68);
            if (tdOff)
            {
                size_t tdp = eio(tdOff, pe);
                if (inb(tdp, 0x10, len))
                {
                    uint32_t v = rd32(buf, tdp + 0x0c);
                    if (v != 0xffffffffu) npc = v;
                }
            }
            // EntityID: entity sub-struct ptr @ part+0x60, EntityID@+0x00 (same as the
            // treasure path). The EMEVD pass joins on this; an enemy with no npc lot but
            // a non-zero EntityID is still a valid EMEVD position anchor (e.g. a scripted
            // boss reward), so we keep the part when EITHER is set.
            uint32_t entityId = 0;
            uint64_t entOff = rd64(buf, pe + 0x60);
            if (entOff)
            {
                size_t ent = eio(entOff, pe);
                if (inb(ent, 0x04, len)) entityId = rd32(buf, ent + 0x00);
            }
            if (entityId == 0xffffffffu) entityId = 0;
            if (npc == 0 && entityId == 0) continue;
            size_t nm = eio(rd64(buf, pe + 0x00), pe);
            if (nm >= len) continue;
            Enemy en;
            en.name = rd_utf16(buf, nm, len);
            en.npcParamId = npc;
            en.entityId = entityId;
            en.pos[0] = rdf(buf, pe + 0x20);
            en.pos[1] = rdf(buf, pe + 0x24);
            en.pos[2] = rdf(buf, pe + 0x28);
            R.enemies.push_back(std::move(en));
        }
    }

    // Spirit-spring regions: enumerate the POINT section (secs[2]) keeping only MountJump (46) /
    // LockedMountJump (54) launch points + Others (-1) named "FakeSpiritSpringJump". Entry layout
    // pinned: subtype@+0x08, name@+0x00 (offset), pos@+0x14. The caller projects each like a part.
    if (wantRegions)
    {
        const Sec &RG = secs[SEC_POINT];
        for (uint32_t i = 0; i < RG.entries; i++)
        {
            size_t re = (size_t)rd64(buf, RG.entryArr + (size_t)i * 8);
            if (!inb(re, 0x20, len)) continue;
            int32_t sub = (int32_t)rd32(buf, re + 0x08);
            bool keep = (sub == REGION_MOUNT_JUMP || sub == REGION_LOCKED_MOUNT_JUMP);
            // Read the name for every region: the spirit-spring "Others" filter AND the Kindling
            // Spirit filter (SFX region named "KindlingSpirit_000N", any subtype) both key on it.
            std::string name;
            {
                size_t nm = eio(rd64(buf, re + 0x00), re);
                if (nm < len) name = rd_utf16(buf, nm, len);
            }
            if (!keep && sub == REGION_OTHERS &&
                name.find("FakeSpiritSpringJump") != std::string::npos)
                keep = true;
            // Kindling Spirits: SFX region "KindlingSpirit_000N" (NOT the lit-variant
            // "KindlingSpiritX_..." — the '_' after "Spirit" excludes it). The caller routes by
            // this name prefix; position = the region pos (= the bake's kindling pos). No flag.
            if (!keep && name.rfind("KindlingSpirit_", 0) == 0)
                keep = true;
            if (!keep) continue;
            Region rg;
            rg.subtype = sub;
            rg.name = std::move(name);
            rg.pos[0] = rdf(buf, re + 0x14);
            rg.pos[1] = rdf(buf, re + 0x18);
            rg.pos[2] = rdf(buf, re + 0x1c);
            R.regions.push_back(std::move(rg));
        }
    }

    R.ok = true;
    return R;
}

// ── EMEVD parse (no-bake EMEVD loot pass, loot_emevd_drops) ───────────────────────────
// 13 template events that award an item lot, each with the (entity, lot) byte offsets into
// the event-init ArgData + the minimum ArgData length. Datamined + DarkScript3-verified in
// tools/extract_all_items.py:646 (TEMPLATE_EVENTS); the pure-bytes parse here matches
// SoulsFormats 500/500 over all ERR event files (tools/probe_emevd_format.py).
namespace {
struct EmevdTemplate { uint32_t eventId; uint16_t entityOff, lotOff, minLen; };
constexpr EmevdTemplate kEmevdTemplates[] = {
    {90005300, 8, 16, 20}, {90005301, 8, 16, 20},                   // scarab / enemy drops
    {90005860, 16, 24, 28}, {90005861, 16, 24, 28}, {90005880, 16, 24, 28}, // boss rewards
    {90005750, 8, 16, 20}, {90005753, 8, 16, 20},                   // NPC quest/dialog rewards
    {90005774, 8, 12, 16},                                          // NPC invasion (pseudo multi)
    {90005792, 20, 24, 28},                                         // hostile NPC defeat
    {90005632, 8, 16, 20},                                          // painting pickups
    {90005110, 8, 20, 24},                                          // great runes
    {90005390, 8, 28, 32},                                          // larval tears (boss morph)
    {90005555, 8, 12, 16},                                          // DLC forging / special
    // Multi-reward "scarab cluster" templates: the FIRST reward is (lot@8, anchorAsset@20). The
    // anchor is an MSB Asset (a scarab/ground object), resolved via the asset-join. Most calls'
    // lot@8 is a non-notable placeholder (filtered by the lot_row_in_table gate downstream), so
    // adding these emits only the few notable first-rewards — the Golden Rune[200] m12_05 (12050510)
    // + Somber Smithing Scadushard m20_01 (20010520) residuals (tools/_probe_loose_blast.py: 0
    // notable collateral). minLen 24 to read @20.
    {90005500, 20, 8, 24}, {90005501, 20, 8, 24},
};
const EmevdTemplate *find_emevd_template(uint32_t eventId)
{
    for (const auto &t : kEmevdTemplates)
        if (t.eventId == eventId) return &t;
    return nullptr;
}
constexpr uint32_t EMEVD_INIT_BANK = 2000; // RunEvent / RunCommonEvent instruction bank
} // namespace

std::vector<EmevdAward> parse_emevd(const uint8_t *buf, size_t len)
{
    std::vector<EmevdAward> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;

    // Header (64-bit EMEVD; all offsets are plain file offsets). Pinned in probe_emevd_format.py.
    uint64_t eventCount   = rd64(buf, 0x10);
    uint64_t eventsOff    = rd64(buf, 0x18);
    uint64_t instrTblOff  = rd64(buf, 0x28);
    uint64_t argsOff      = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;

    constexpr size_t EVENT_SZ = 0x30;  // {id@0, instrCount@8, instrOffset@10, ...}
    constexpr size_t INSTR_SZ = 0x20;  // {bank@0, id@4, argLen@8, argOffset@10(int32), ...}

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);  // byte offset into the instruction table
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);  // -1 when no args
            if (argOff < 0 || argLen < 8) continue;
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            uint32_t eventId = rd32(buf, a + 4);  // arg[4..8] = the invoked event id
            const EmevdTemplate *t = find_emevd_template(eventId);
            if (!t) continue;
            if (argLen < t->minLen) continue;
            uint32_t entity = rd32(buf, a + t->entityOff);
            uint32_t lot    = rd32(buf, a + t->lotOff);
            if ((int32_t)lot > 0 && (int32_t)entity > 0)
                out.push_back({entity, lot});
        }
    }
    return out;
}

// Quest-NPC concluded/register handler: InitializeCommonEvent(0, 90005702, entity, concluded,
// reg_lo, reg_hi). Same 64-bit EMEVD layout as parse_emevd; args at a+8/+12/+16/+20. Runtime
// port of tools/extract_quest_npcs.py's 90005702 mine — the whole quest-NPC table comes from
// this ONE template, so it is correct for any mod that adds/removes/relocates NPCs (no bake).
std::vector<QuestNpcEmevd> parse_emevd_quest_npcs(const uint8_t *buf, size_t len)
{
    std::vector<QuestNpcEmevd> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;

    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;

    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;
    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 24) continue;  // need reg_hi @ a+20..24
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            if (rd32(buf, a + 4) != 90005702u) continue;  // the quest-NPC concluded handler
            QuestNpcEmevd q{ rd32(buf, a + 8), rd32(buf, a + 12), rd32(buf, a + 16), rd32(buf, a + 20) };
            if ((int32_t)q.entity > 0 && (int32_t)q.concluded > 0 && q.regLo <= q.regHi)
                out.push_back(q);
        }
    }
    return out;
}

// Flag-award templates (World-feature graying) — distinct from the item-lot templates above:
// the event init carries (entity, FLAG) instead of (entity, lot). Hero's Tomb instruction
// statue = template 90005683 (args: visgate@8, entity@12, sfx@16, activated_flag@20; matches
// tools/generate_hero_tomb_statues.py). EmevdTemplate's `lotOff` field is reused as the flag offset.
namespace {
constexpr EmevdTemplate kEmevdFlagTemplates[] = {
    {90005683, 12, 20, 24},  // Hero's Tomb statue: entity@+12, activated flag@+20, minLen 24
    {90005792, 20, 8, 24},   // Hostile NPC defeated: entity@+20 (X12_4), defeat flag@+8 (X0_4)
    {90006051, 8, 12, 16},   // Seal puzzle per-seal: seal entity@+8 (X0_4), activation flag@+12
                             // (X4_4); params (seal_eid, flag, sfx, group) — tools/extract_seal_puzzles.py
    // Bespoke "extra puzzle" events — no shared template id (each is a per-map event), same
    // (entity, flag) shape as the seals. Params are 0-based from byte 8 (X0_4=+8). Mirrors
    // tools/extract_seal_puzzles.py:_EXTRA_PUZZLES. The world-feature pass joins entity→flag
    // for the chalice/lantern asset models (AEG099_047 / AEG237_055) via seal_emevd self-gate.
    {1049392302, 8, 12, 16}, // Sellia chalice (big):   entity@X0_4(+8), lit flag@X4_4(+12)
    {1049392303, 8, 12, 16}, // Sellia chalice (small)
    {1050392303, 8, 12, 16}, // Sellia chalice (m60_50)
    {12022601, 12, 8, 16},   // Siofra lower-layer lantern: anchor asset@X4_4(+12), lit flag@X0_4(+8)
    {12022621, 12, 8, 16},   // Siofra upper-layer lantern
    {1048572370, 8, 16, 20}, // Snow Town seal-release statue (4×): entity@X0_4(+8), lit flag@X8_4(+16);
                             // arglen 20, params (entity, sfx_eid, lit_flag). The AEG110_029 assets live
                             // ONLY in LOD supertile m60_24_28_01 → recovered by load_lod_feature_assets.
};
const EmevdTemplate *find_emevd_flag_template(uint32_t eventId)
{
    for (const auto &t : kEmevdFlagTemplates)
        if (t.eventId == eventId) return &t;
    return nullptr;
}
} // namespace

std::vector<std::pair<uint32_t, uint32_t>> parse_emevd_flag_awards(const uint8_t *buf, size_t len)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;

    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 8) continue;
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            uint32_t eventId = rd32(buf, a + 4);
            const EmevdTemplate *t = find_emevd_flag_template(eventId);
            if (!t) continue;
            if (argLen < t->minLen) continue;
            uint32_t entity = rd32(buf, a + t->entityOff);
            uint32_t flag   = rd32(buf, a + t->lotOff);  // lotOff reused as the flag offset
            if ((int32_t)entity > 0 && (int32_t)flag > 0)
                out.emplace_back(entity, flag);
        }
    }
    return out;
}

// Portal / sending-gate entities — a bank-2000 init of common warp template 90005605 carries the
// gate entity at arg[2] (X0_4 @ +8). No flag (portals never "complete"), so this returns just the
// entity id per call. RE-verified: 23 distinct AEG099_510 gates across the world (tools/
// _probe_portal_verify.py). Same pinned 64-bit layout as parse_emevd_flag_awards. The disk pass
// dedups by entity (LOD _00/_10 tiles repeat the same call). See windows_portal_aeg_re_findings.md.
std::vector<uint32_t> parse_emevd_portal_gates(const uint8_t *buf, size_t len)
{
    std::vector<uint32_t> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;
    constexpr uint32_t kPortalTemplate = 90005605u;
    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;
    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 12) continue;   // need arg[2] @ a+8..12
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            if (rd32(buf, a + 4) != kPortalTemplate) continue;
            uint32_t entity = rd32(buf, a + 8);        // arg[2] = gate entity (X0_4)
            if ((int32_t)entity > 0) out.push_back(entity);
        }
    }
    return out;
}

// Painting events (World-feature graying) — like parse_emevd_flag_awards but the eventId is
// NOT a fixed template id: the DLC paintings each have a UNIQUE map-specific template (e.g.
// 2045432550), so a template TABLE can't catch them. Instead detect by the FLAG range. Two
// shapes (mirrors tools/generate_paintings.py exactly):
//   • base template 90005632: args = [_, tmpl, FLAG@idx2, entity@idx3, ...]  (the 4 Asset paintings)
//   • everything else (90005633 + DLC): args = [_, tmpl, _, FLAG@idx3, entity@idx4, ...]
//     kept only when args[idx3] is itself in the painting-flag range (the 7 ghost-painter Enemies).
// Returns (entity -> flag), both > 0, flag in [580000,580199]. The painting disk pass joins the
// entity to its MSB Asset OR Enemy position (both carry EntityID). Same pinned 64-bit layout.
std::vector<std::pair<uint32_t, uint32_t>> parse_emevd_paintings(const uint8_t *buf, size_t len)
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;

    constexpr uint32_t kFlagMin = 580000u, kFlagMax = 580199u;
    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 20) continue;  // need 5 int32 args (entity@idx4 = byte 16)
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            uint32_t tmpl = rd32(buf, a + 4);  // args[1] = template / eventId
            uint32_t flag = 0, entity = 0;
            if (tmpl == 90005632)
            {
                flag   = rd32(buf, a + 8);   // args[2]
                entity = rd32(buf, a + 12);  // args[3]
            }
            else
            {
                uint32_t cand = rd32(buf, a + 12);  // args[3]
                if (cand >= kFlagMin && cand <= kFlagMax)
                {
                    flag   = cand;
                    entity = rd32(buf, a + 16);  // args[4]
                }
            }
            if (flag >= kFlagMin && flag <= kFlagMax && (int32_t)entity > 0)
                out.emplace_back(entity, flag);
        }
    }
    return out;
}

// Gesture-spawn events (the no-bake Loot - Gestures source). Fixed common template 90005570,
// args = [_, tmpl, flag@idx2, gestureParam@idx3, entity@idx4, ...]. Mirrors tools/generate_gestures.py.
// Returns one GestureRef per call with entity > 0; the caller joins the entity to its MSB Asset
// position and resolves the name via GestureParam[gestureParam].itemId (live). Same pinned layout.
std::vector<GestureRef> parse_emevd_gestures(const uint8_t *buf, size_t len)
{
    std::vector<GestureRef> out;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return out;

    constexpr uint32_t kGestureTemplate = 90005570u;
    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return out;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            if (rd32(buf, ins + 0x00) != EMEVD_INIT_BANK) continue;
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 20) continue;  // need 5 int32 args (entity@idx4 = byte 16)
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;
            if (rd32(buf, a + 4) != kGestureTemplate) continue;  // args[1] = template / eventId
            GestureRef g;
            g.flag         = rd32(buf, a + 8);   // args[2]
            g.gestureParam = rd32(buf, a + 12);  // args[3]
            g.entityId     = rd32(buf, a + 16);  // args[4]
            if ((int32_t)g.entityId > 0)
                out.push_back(g);
        }
    }
    return out;
}

EmevdParse parse_emevd_full(const uint8_t *buf, size_t len)
{
    EmevdParse R;
    if (len < 0x80 || std::memcmp(buf, "EVD\0", 4) != 0) return R;

    uint64_t eventCount  = rd64(buf, 0x10);
    uint64_t eventsOff   = rd64(buf, 0x18);
    uint64_t instrTblOff = rd64(buf, 0x28);
    uint64_t argsOff     = rd64(buf, 0x78);
    if (eventCount > 1000000u) return R;
    constexpr size_t EVENT_SZ = 0x30, INSTR_SZ = 0x20;
    // entity-id range filter for setter-event candidates (the caller intersects with the
    // real MSB enemy entities, so this only needs to drop obvious non-entities: flags,
    // small enums, item ids stay possible but get filtered out by the intersection).
    constexpr uint32_t kEntMin = 1000u, kEntMax = 0x7fffffffu;

    for (uint64_t i = 0; i < eventCount; ++i)
    {
        size_t e = (size_t)eventsOff + (size_t)i * EVENT_SZ;
        if (!inb(e, EVENT_SZ, len)) break;
        uint64_t instrCount  = rd64(buf, e + 0x08);
        uint64_t instrOffset = rd64(buf, e + 0x10);
        size_t base = (size_t)instrTblOff + (size_t)instrOffset;
        if (instrCount > 1000000u) continue;

        std::vector<uint32_t> setFlags;     // SetEventFlag(.,1) in this event
        std::vector<uint32_t> candidates;   // entity-range values referenced in this event
        for (uint64_t j = 0; j < instrCount; ++j)
        {
            size_t ins = base + (size_t)j * INSTR_SZ;
            if (!inb(ins, INSTR_SZ, len)) break;
            uint32_t bank = rd32(buf, ins + 0x00);
            uint32_t iid  = rd32(buf, ins + 0x04);
            uint64_t argLen = rd64(buf, ins + 0x08);
            int32_t  argOff = (int32_t)rd32(buf, ins + 0x10);
            if (argOff < 0 || argLen < 4) continue;
            size_t a = (size_t)argsOff + (size_t)argOff;
            if (!inb(a, (size_t)argLen, len)) continue;

            // candidate entity values: every 4-byte window in this instruction's args
            for (size_t k = 0; k + 4 <= (size_t)argLen; k += 4)
            {
                uint32_t v = rd32(buf, a + k);
                if (v >= kEntMin && v <= kEntMax) candidates.push_back(v);
            }
            if (argLen < 8) continue;
            uint32_t eventId = rd32(buf, a + 4);
            if (bank == EMEVD_INIT_BANK)
            {
                // mechanism A: direct template award
                if (const EmevdTemplate *t = find_emevd_template(eventId))
                {
                    if (argLen >= t->minLen)
                    {
                        uint32_t entity = rd32(buf, a + t->entityOff);
                        uint32_t lot    = rd32(buf, a + t->lotOff);
                        if ((int32_t)lot > 0 && (int32_t)entity > 0)
                        {
                            EmevdAward aw{entity, lot, {}};
                            // Loose-anchor candidates: the init's OTHER map-entity-range windows,
                            // nearest to the lot arg first. Used only if `entity` doesn't resolve to
                            // a position (the documented entity is a non-positionable logic id, the
                            // real anchor is elsewhere — Taylew/Viridian). Map entity ids are ≥ 1e6.
                            const size_t nWin = (size_t)argLen / 4, lotIdx = t->lotOff / 4;
                            std::vector<std::pair<size_t, uint32_t>> cand;  // (distance to lot, value)
                            for (size_t k = 0; k < nWin; ++k)
                            {
                                if (k == lotIdx || k == 1) continue;  // skip the lot + the eventId arg
                                uint32_t v = rd32(buf, a + k * 4);
                                if (v < 1000000u || v == entity || v == lot) continue;
                                cand.emplace_back(k > lotIdx ? k - lotIdx : lotIdx - k, v);
                            }
                            std::sort(cand.begin(), cand.end());
                            for (auto &c : cand)
                                if (std::find(aw.anchors.begin(), aw.anchors.end(), c.second) ==
                                    aw.anchors.end())
                                    aw.anchors.push_back(c.second);
                            R.direct.push_back(std::move(aw));
                        }
                    }
                }
                // Per-tile "enemy-death item award": a callee >= 1e9 (a per-tile event id, NOT a
                // kEmevdTemplate) awards a lot when an enemy dies. entity@X0_4 (a+8), lot@idx(n-2)
                // (a+argLen-8) — a consistent single-pair layout (verified on the 12 uncovered Golden
                // Runes, tools/_probe_emevd_precise.py: 21 lots, 0 already-covered, 12/12 GR). The
                // downstream entity→disk-ENEMY join filters out asset-entity chests (the 395 over-match)
                // and the lot-coverage dedup drops anything another pass placed → only the uncovered
                // enemy-death awards survive as markers. Kept separate from R.direct for an observable count.
                else if (eventId >= 1000000000u && argLen >= 16)
                {
                    const size_t nWin = (size_t)argLen / 4;
                    // (a) the calibrated single-pair award: entity@8, lot@idx(n-2).
                    uint32_t entity = rd32(buf, a + 8);
                    uint32_t lot    = rd32(buf, a + (size_t)argLen - 8);
                    if ((int32_t)entity > 0 && (int32_t)lot > 0 && entity != lot)
                        R.perTileEnemyAward.push_back({entity, lot, {}});
                    // (b) the "kill-the-group → award" variant puts the lot at the LAST arg (idx n-1)
                    // and a constant 1 at idx(n-2), so (a) reads junk and misses it. Its anchor is NOT
                    // at a fixed offset → carry the nearest enemy-range windows as loose anchors; the
                    // join resolves them ENEMY-ONLY (allowAsset=false for perTile — an asset anchor here
                    // re-opens the ~395 asset-entity-chest over-match). Recovers the 2 Larval Tear
                    // residuals + 2 more real items (another Larval Tear + an armour), 0 notable phantoms
                    // — the lot_row_in_table gate filters the 370 non-item idx(n-1) values
                    // (tools/_probe_pertile_lastlot.py). entity 0 → the join uses the anchors directly.
                    uint32_t lotLast = rd32(buf, a + (size_t)argLen - 4);
                    if ((int32_t)lotLast > 0 && lotLast != lot)
                    {
                        EmevdAward aw{0u, lotLast, {}};
                        const size_t lotIdx = nWin - 1;
                        std::vector<std::pair<size_t, uint32_t>> cand;  // (distance to lot, value)
                        for (size_t k = 0; k < nWin; ++k)
                        {
                            if (k == lotIdx || k == 1) continue;  // skip the lot + the eventId arg
                            uint32_t v = rd32(buf, a + k * 4);
                            if (v < 1000000u || v == lotLast) continue;
                            cand.emplace_back(lotIdx - k, v);  // k < lotIdx (lot is last)
                        }
                        std::sort(cand.begin(), cand.end());
                        for (auto &c : cand)
                            if (std::find(aw.anchors.begin(), aw.anchors.end(), c.second) ==
                                aw.anchors.end())
                                aw.anchors.push_back(c.second);
                        if (!aw.anchors.empty()) R.perTileEnemyAward.push_back(std::move(aw));
                    }
                }
                // mechanism B input: RunEvent(2000:00) binding flag→lot via callee 1200
                if (iid == 0 && eventId == 1200 && argLen >= 16)
                {
                    uint32_t flag = rd32(buf, a + 8);
                    uint32_t lot  = rd32(buf, a + 12);
                    if ((int32_t)flag > 0 && (int32_t)lot > 0)
                        R.runEvent1200.push_back({flag, lot});
                }
                // Boss-reward templates 90005860/61/80: defeatFlag@8, bossEntity@16, baseLot@24.
                // Collected here (per-map) so the caller joins defeatFlag→boss (or the explicit
                // bossEntity@16) + walks baseLot's chain for the Rune/Ember Piece. minLen 28.
                if ((eventId == 90005860 || eventId == 90005861 || eventId == 90005880) && argLen >= 28)
                {
                    uint32_t flag   = rd32(buf, a + 8);   // X0_4 = boss defeat flag
                    uint32_t entity = rd32(buf, a + 16);  // X8_4 = explicit boss MSB EntityID
                    uint32_t lot    = rd32(buf, a + 24);  // base ItemLotParam (piece = base+1/+2)
                    if ((int32_t)flag > 0 && (int32_t)lot > 0)
                        R.bossFlagLot.push_back({flag, lot, (int32_t)entity > 0 ? entity : 0u});
                }
            }
            // mechanism B input: SetEventFlag(2003:66) / SetNetworkEventFlag(2003:69), state==1
            else if (bank == 2003 && (iid == 66 || iid == 69) && argLen >= 12)
            {
                uint32_t flag  = rd32(buf, a + 4);
                int32_t  state = (int32_t)rd32(buf, a + 8);
                if (state == 1 && (int32_t)flag > 0) setFlags.push_back(flag);
            }
        }
        if (!setFlags.empty())
            R.setters.push_back({std::move(setFlags), std::move(candidates)});
    }
    return R;
}

std::vector<uint8_t> dcx_decompress(const uint8_t *d, size_t len, bool *isKrak,
                                    OodleDecompressFn oodle)
{
    if (isKrak) *isKrak = false;
    std::vector<uint8_t> out;
    if (len < 0x50 || std::memcmp(d, "DCX\0", 4) != 0) return out;
    if (std::memcmp(d + 0x18, "DCS\0", 4) != 0) return out;

    auto be32 = [&](size_t o) {
        return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 |
               (uint32_t)d[o + 3];
    };
    uint32_t uncomp = be32(0x1c); // DCS +4 = uncompressed size (big-endian)

    // Format block: "DCP\0" tag @0x24, 4-char format @0x28 ("DFLT" zlib | "KRAK" Oodle).
    bool krak = false;
    if (std::memcmp(d + 0x24, "DCP\0", 4) == 0)
    {
        if (std::memcmp(d + 0x28, "KRAK", 4) == 0) krak = true;
        else if (std::memcmp(d + 0x28, "DFLT", 4) != 0) return out;
    }
    if (krak && isKrak) *isKrak = true;

    // Compressed stream starts at find("DCA\0") + 8 (DFLT zlib or KRAK Oodle alike).
    size_t dca = SIZE_MAX;
    for (size_t i = 0x28; i + 4 <= len; i++)
        if (d[i] == 'D' && d[i + 1] == 'C' && d[i + 2] == 'A' && d[i + 3] == 0) { dca = i; break; }
    if (dca == SIZE_MAX) return out;
    size_t zoff = dca + 8;
    if (zoff >= len || uncomp == 0 || uncomp > 256u * 1024 * 1024) return out;

    out.resize(uncomp);
    if (krak)
    {
        // KRAK needs the game's Oodle; without it the caller skips this map (isKrak set).
        if (!oodle) { out.clear(); return out; }
        long long got = oodle(d + zoff, (long long)(len - zoff), out.data(), (long long)uncomp,
                              /*fuzz=*/1, /*crc=*/0, /*verbose=*/0, nullptr, 0, nullptr, nullptr,
                              nullptr, 0, /*threadPhase=*/3 /* OodleLZ_Decode_Unthreaded */);
        if (got != (long long)uncomp) { out.clear(); return out; }
        return out;
    }

    int got = stbi_zlib_decode_buffer((char *)out.data(), (int)uncomp,
                                      (const char *)(d + zoff), (int)(len - zoff));
    if (got <= 0) { out.clear(); return out; }
    out.resize((size_t)got);
    return out;
}

bool tpf_find_texture(const uint8_t *buf, size_t n, const char *name, size_t &ddsOff,
                      size_t &ddsLen)
{
    ddsOff = ddsLen = 0;
    if (!buf || !name || n < 16 || !(buf[0] == 'T' && buf[1] == 'P' && buf[2] == 'F' && buf[3] == 0))
        return false;
    auto rd32 = [&](size_t p) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, buf + p, 4);
        return v;
    };
    uint32_t fileCount = rd32(8);
    uint8_t  platform = buf[12];
    uint8_t  encoding = buf[14];
    if (platform != 0 || fileCount == 0 || fileCount > 100000)  // 0 = PC only
        return false;

    size_t pos = 16;
    for (uint32_t i = 0; i < fileCount; ++i)
    {
        if (pos + 20 > n)
            return false;
        uint32_t fileOffset = rd32(pos);
        uint32_t fileSize   = rd32(pos + 4);
        uint8_t  flags1     = buf[pos + 11];
        uint32_t nameOffset = rd32(pos + 12);
        uint32_t hasFloat   = rd32(pos + 16);
        pos += 20;
        if (hasFloat)  // FloatStruct: i32 Unk00, i32 length, float[length]
        {
            if (pos + 8 > n)
                return false;
            uint32_t flen = rd32(pos + 4);
            pos += 8 + (size_t)flen * 4;
        }
        // Compare the UTF-16LE name at nameOffset to the ASCII `name` (low byte == char,
        // high byte == 0 for ASCII names like SB_Icon_00).
        bool match = true;
        for (const char *c = name;; ++c)
        {
            size_t np = (size_t)nameOffset + (size_t)(c - name) * 2;
            if (np + 1 >= n) { match = false; break; }
            uint8_t lo = buf[np], hi = buf[np + 1];
            if (*c == '\0') { match = (lo == 0 && hi == 0); break; }
            if (lo != (uint8_t)*c || hi != 0) { match = false; break; }
        }
        if (!match)
            continue;
        if (flags1 == 2 || flags1 == 3)  // per-entry DCP_EDGE — not supported (menu sheets are raw)
            return false;
        if ((size_t)fileOffset + fileSize > n || fileSize < 4)
            return false;
        ddsOff = fileOffset;
        ddsLen = fileSize;
        return true;
    }
    return false;
}

} // namespace goblin::msbe
