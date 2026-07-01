#include "goblin_inject.hpp"
#include "goblin_config.hpp"
#include "goblin_bench.hpp"
#include "goblin_messages.hpp"
#include "modutils.hpp"
#include "re_signatures.hpp"
#include "from/params.hpp"
#include "worldmap/loot_disk.hpp"  // read_game_file_decompressed (no-bake item-icon layout source)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>
#include <windows.h>

//
// Icon-texture harvest + GPU-icon registry — split out of goblin_inject.cpp
// 2026-07-01 (docs/plans/goblin_inject_refactor_plan.md PR 1). Pure relocation,
// no logic changes. Owns: CreateImage capture/probe, loaded-image enumeration,
// the harvest-via-find-hook + proactive repo-walk harvest, the TWIN-map walk
// for WorldMapPoint pin icons, the central GPU-icon registry + CreateImage
// force-bind primitives, the OodleLZ_Decompress hook, and the item-icon
// layout parsed from the sblytbnd XML. Declarations stay in goblin_inject.hpp
// (facade kept, zero call-site churn — 30 public functions here are called
// from goblin_overlay.cpp, goblin_worldmap_probe.cpp, goblin_markers.cpp,
// goblin_messages.cpp, dllmain.cpp, worldmap/*).
//
// The one real cross-boundary coupling found during the split: goblin_inject.cpp's
// warp_pin_detour (native discovered-grace pin suppression, stays behind) needs
// icon_rpm_i32/icon_rpm_ptr, two 5-line RPM-wrapper helpers defined in this block.
// Per the same per-file-copy convention PR0 already established (goblin_markers.cpp/
// goblin_kindling.cpp each keep their own AOB/RPM helper copies), goblin_inject.cpp
// keeps its OWN local duplicate of these two helpers rather than sharing via a header.
//

// ── Icon-texture probe (config dump_icon_textures) ───────────────────────────────────
// Hooks CSScaleformImageCreator::CreateImage (FUN_140d6bbc0): builds each GFx image from a
// TPF import → a CS::CSTextureImage carrying the sprite RECT (+0x74/+0x7c/+0x3c/+0x40, dims
// +0x2c/+0x30) + backing GXTexture2D (+0x38 → ID3D12Resource +0x40). RE findings §2/§7. Logs
// the created image's rect/dims + backing texture + the import args so we can map iconIds →
// sprite sub-rects. Read-only (RPM); generous arg list forwarded (x64 caller-clean → extra
// args are harmless even if CreateImage takes fewer).
namespace
{
using create_image_fn = void *(__fastcall *)(void *, void *, void *, void *, void *, void *,
                                             void *, void *);
create_image_fn g_create_image_orig = nullptr;

// CreateImage context capture (RE §5g) — populated by create_image_detour, consumed by
// run_create_icon to replay the GFx per-image bind for an arbitrary symbol. a0=param_1 (creator this),
// a1=param_2, a2=param_3 (symbol desc: ASCII name @ (*a2 & ~3)+0xc, format "%hS").
std::atomic<void *> g_ci_p1{nullptr};
std::atomic<void *> g_ci_p2{nullptr};
std::atomic<int> g_ci_logn{0};
char g_ci_last[96] = {0};   // most-recent live symbol name (e.g. "img://KG_R1") for replay control test

inline bool icon_rpm_i32(uintptr_t a, int &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 4, &n) && n == 4;
}
inline bool icon_rpm_ptr(uintptr_t a, uintptr_t &out)
{
    SIZE_T n = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), (void *)a, &out, 8, &n) && n == 8;
}

// Bounded BFS over the CS object graph from `root` (follow er-base-vtable'd ptr fields)
// for the GXTexture2D / CSGxTexture vtable → the backing GPU texture. Returns its addr
// (+ out vtable/depth), 0 if not found. Used at load (usually fails — texture bound
// lazily) and on a map-open frame from root = *(img+0x10) (the now-bound Render::Texture).
uintptr_t icon_find_gpu_tex(uintptr_t root, uintptr_t base, uintptr_t &out_vt, int &out_depth)
{
    // ONLY GXTexture2D (0x2f05928) is the terminal — it carries the ID3D12Resource at +0x40.
    // CSGxTexture (0x2b761b0) is a WRAPPER whose +0x40 is a name string, so we expand THROUGH
    // it (and every other CS object) rather than stopping on it.
    const uintptr_t WANT_GX = base + 0x2f05928;
    std::vector<std::pair<uintptr_t, int>> q;
    std::vector<uintptr_t> seen;
    q.push_back({root, 0}); seen.push_back(root);
    for (size_t qi = 0; qi < q.size() && qi < 600; ++qi)
    {
        uintptr_t node = q[qi].first; int d = q[qi].second;
        for (int o = 0x00; o <= 0x120; o += 8)
        {
            uintptr_t p = 0;
            if (!icon_rpm_ptr(node + o, p) || p < 0x10000 || p >= 0x7fffffffffffULL) continue;
            uintptr_t vt = 0;
            if (!icon_rpm_ptr(p, vt)) continue;
            if (vt == WANT_GX) { out_vt = vt; out_depth = d + 1; return p; }
            if (d < 7 && vt > base && vt < base + 0x6000000 && q.size() < 600)
            {
                bool dup = false;
                for (uintptr_t s : seen) if (s == p) { dup = true; break; }
                if (!dup) { seen.push_back(p); q.push_back({p, d + 1}); }
            }
        }
    }
    out_vt = 0; out_depth = -1;
    return 0;
}

// Try to read a printable string at `addr` — narrow (char) first, then wide (char,0,char,0).
// Returns "" if no run of >=3 printable ASCII. Used to find the GFx import NAME the image was
// built from (findings §4: CreateImage builds from an import descriptor; the resolve path
// formats L"%s_ptl" from its wide name → label iconId by that name).
std::string icon_try_str(uintptr_t addr)
{
    if (!addr || addr < 0x10000 || addr >= 0x7fffffffffffULL) return {};
    unsigned char buf[96] = {0};
    SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)addr, buf, sizeof(buf) - 1, &n) || n < 4)
        return {};
    auto printable = [](unsigned char c) { return (c >= 0x20 && c < 0x7f); };
    // narrow
    int run = 0; for (int i = 0; i < (int)n && printable(buf[i]); ++i) run++;
    if (run >= 4) return std::string((char *)buf, run);
    // wide (every other byte 0)
    std::string w; for (int i = 0; i + 1 < (int)n; i += 2)
    {
        if (printable(buf[i]) && buf[i + 1] == 0) w.push_back((char)buf[i]); else break;
    }
    if (w.size() >= 4) return w;
    return {};
}

// Registry of created CSTextureImages (load-time) so the live dump can re-read each
// img+0x10 once the world map is open and the GPU texture is bound. Capped; sheet-sized
// images only (icon sheets are 512×512 / 2048×1024 — skip tiny/huge GFx images).
struct IconImg { uintptr_t img; int x0, y0, x1, y1, w, h; std::string name; };
std::vector<IconImg> g_icon_imgs;
std::vector<uintptr_t> g_icon_sheets; // unique ID3D12Resource per TPF sheet
bool g_icon_live_done = false;
bool g_icon_detail_done = false;

// One-shot full dump of the FIRST bound Render::Texture (img+0x10). The BFS proved
// non-deterministic (wanders into a shared global some runs, misses others), so dump the
// Scaleform Render::Texture's whole layout: every ptr field (tagged GXTexture2D /
// CSGxTexture / other CS-obj, + one-level child that points at GXTexture2D) and every
// small int (dims / the tex+0x3c size). Reveals the DETERMINISTIC renderTex→GPU-texture
// offset to replace the BFS, and whether all images share one atlas.
void icon_detail_dump(uintptr_t rtex, uintptr_t base)
{
    if (g_icon_detail_done || !rtex) return;
    g_icon_detail_done = true;
    const uintptr_t WANT_GX = base + 0x2f05928, WANT_CSGX = base + 0x2b761b0;
    spdlog::info("[ICONTEX-DET] renderTex={:#x} full layout dump:", rtex);
    for (int o = 0; o <= 0xB8; o += 8)
    {
        uintptr_t p = 0;
        if (!icon_rpm_ptr(rtex + o, p)) continue;
        if (p > 0x10000 && p < 0x7fffffffffffULL)
        {
            uintptr_t vt = 0; icon_rpm_ptr(p, vt);
            const char *tag = vt == WANT_GX ? " <GXTexture2D>"
                            : vt == WANT_CSGX ? " <CSGxTexture>"
                            : (vt > base && vt < base + 0x6000000) ? " <CSobj>" : "";
            spdlog::info("[ICONTEX-DET]  rtex+{:#x} = ptr {:#x} vtRVA={:#x}{}",
                         o, p, vt > base ? vt - base : 0, tag);
            // one level down: a child field pointing straight at the GXTexture2D
            if (vt > base && vt < base + 0x6000000)
                for (int o2 = 0; o2 <= 0x70; o2 += 8)
                {
                    uintptr_t c = 0; if (!icon_rpm_ptr(p + o2, c) || c < 0x10000) continue;
                    uintptr_t cv = 0; icon_rpm_ptr(c, cv);
                    if (cv == WANT_GX)
                        spdlog::info("[ICONTEX-DET]    -> [{:#x}+{:#x}] = GXTexture2D {:#x}", p, o2, c);
                }
        }
        else
        {
            int lo = (int)(p & 0xffffffff), hi = (int)(p >> 32);
            if ((lo > 0 && lo <= 8192) || (hi > 0 && hi <= 8192))
                spdlog::info("[ICONTEX-DET]  rtex+{:#x} = ints ({}, {})", o, lo, hi);
        }
    }
    // rtex+0x70 = the D3D texture HAL object (non-module vtable, repeated across the texture
    // list → a real D3D/driver type). Deep-dump it: its fields + ints, hunting the actual
    // ID3D12Resource (a ptr whose vtable is OUTSIDE the ER module = d3d12.dll / vkd3d) and the
    // texture dims/format. mod range = [base, base+0x6000000).
    const uintptr_t mod_lo = base, mod_hi = base + 0x6000000;
    uintptr_t hal = 0;
    if (icon_rpm_ptr(rtex + 0x70, hal) && hal > 0x10000 && hal < 0x7fffffffffffULL)
    {
        uintptr_t halvt = 0; icon_rpm_ptr(hal, halvt);
        spdlog::info("[ICONTEX-DET] -- HAL tex rtex+0x70 = {:#x} vt={:#x} fields:", hal, halvt);
        for (int o = 0; o <= 0xC0; o += 8)
        {
            uintptr_t p = 0;
            if (!icon_rpm_ptr(hal + o, p)) continue;
            if (p > 0x10000 && p < 0x7fffffffffffULL)
            {
                uintptr_t vt = 0; bool hasvt = icon_rpm_ptr(p, vt) && vt > 0x10000;
                bool in_mod = (vt >= mod_lo && vt < mod_hi);
                const char *tag = !hasvt ? " <data?>"
                                : in_mod ? " <CSobj>" : " <NON-MODULE vt = D3D12/COM?>";
                spdlog::info("[ICONTEX-DET]    hal+{:#x} = ptr {:#x} vt={:#x}{}",
                             o, p, hasvt ? (in_mod ? vt - base : vt) : 0, tag);
            }
            else
            {
                int lo = (int)(p & 0xffffffff), hi = (int)(p >> 32);
                if ((lo > 0 && lo <= 8192) || (hi > 0 && hi <= 8192))
                    spdlog::info("[ICONTEX-DET]    hal+{:#x} = ints ({}, {})", o, lo, hi);
            }
        }
    }
}

// Live anchors: the numeric iconIds the menu actually DRAW (MENU_FL_<N>) as the user browses.
static std::vector<uint16_t> g_menu_iconids;

// Correlate the live-captured iconIds against the EquipParam tables: for each param, the offset
// whose u16 column CONTAINS the most captured iconIds = that param's iconId offset (every drawn
// item's iconId lives at the same offset). This is the anchored self-calibration that the abstract
// distinct/density heuristics couldn't do — the anchors are real, drawn items. Hover a few items of
// each type → [ICONFIND] converges. Dev-only (dump_icon_textures).
static void correlate_menu_iconids()
{
    std::set<uint16_t> ids(g_menu_iconids.begin(), g_menu_iconids.end());
    if (ids.empty())
        return;
    // sample a few captured ids in the log so we can value-scan manually if needed
    std::string sample;
    { int k = 0; for (uint16_t v : ids) { if (k++) sample += ","; sample += std::to_string(v); if (k >= 8) break; } }
    spdlog::info("[ICONFIND] correlating {} captured iconIds (e.g. {}) vs EquipParams:",
                 (int)ids.size(), sample);
    struct P { const wchar_t *name; const char *tag; };
    static const P params[] = {
        {L"EquipParamWeapon", "Weapon"}, {L"EquipParamProtector", "Protector"},
        {L"EquipParamAccessory", "Accessory"}, {L"EquipParamGoods", "Goods"},
        {L"EquipParamGem", "Gem"}, {L"Magic", "Magic"},
    };
    int totalBest = 0;
    for (const P &p : params)
    {
        try
        {
            std::vector<uintptr_t> bases;
            for (auto row : from::params::get_param<uint8_t>(p.name))
                bases.push_back(reinterpret_cast<uintptr_t>(&row.second));
            if (bases.size() < 2) { spdlog::info("[ICONFIND]   {} not loaded", p.tag); continue; }
            uintptr_t stride = bases[1] - bases[0];
            if (stride < 4 || stride > 0x2000) continue;
            int bestOff = -1, bestHits = 0;
            for (uintptr_t off = 0; off + 2 <= stride; ++off)
            {
                std::set<uint16_t> hit;
                for (uintptr_t b : bases)
                {
                    uint16_t v = *reinterpret_cast<uint16_t *>(b + off);
                    if (ids.count(v)) hit.insert(v);
                }
                if ((int)hit.size() > bestHits) { bestHits = (int)hit.size(); bestOff = (int)off; }
            }
            totalBest += bestHits;
            spdlog::info("[ICONFIND]   {} bestOff=0x{:x} matches {}/{}", p.tag, bestOff, bestHits, (int)ids.size());
        }
        catch (...) { spdlog::info("[ICONFIND]   {} not loaded", p.tag); }
    }
    if (totalBest == 0)
        spdlog::info("[ICONFIND] NO captured iconId appears in ANY EquipParam column → MENU_FL_<id> "
                     "is a TRANSFORMED id, not the raw EquipParam.iconId.");
}

void icon_log_image(uintptr_t img, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    if (!goblin::config::dumpIconTextures || !img)
        return;
    static int logged = 0; // gates only the verbose [ICONTEX] line; [ICONMAP] is uncapped
    // Rect SOLVED: x0,y0,x1,y1 contiguous at +0x74/+0x78/+0x7c/+0x80; dims +0x84/+0x88.
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, w = 0, h = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    icon_rpm_i32(img + 0x84, w);  icon_rpm_i32(img + 0x88, h);
    // Register sheet-sized images so the live dump can re-read img+0x10 once the map is
    // open (the GPU texture binds LAZILY on first render — findings §2; the BFS here at
    // load time finds nothing, which is expected). Icon sheets are 512×512 / 2048×1024.
    // The GFx import NAME (SOLVED): img+0x40 -> '<symbol>_ptl' string (the resolve fn formats
    // L"%s_ptl"). ERR map icons = 'SB_ERR_*'; 'KG_*' = controller button glyphs (skip).
    uintptr_t name_ptr = 0; icon_rpm_ptr(img + 0x40, name_ptr);
    std::string name = icon_try_str(name_ptr);
    // Register sheet-sized images so the live dump can re-read img+0x10 once the map is open
    // (the GPU texture binds LAZILY on first render). Icon sheets are 512×512 / 2048×1024.
    // Sheet-sized images (≥256), OR any SB_ERR_*/MENU_MAP_* (the grace/pin _ptl may report ICON size
    // at +0x84/+0x88, not the sheet — don't let the ≥256 gate drop the forced grace candidates).
    if (((w >= 256 && h >= 256) || name.rfind("SB_ERR_", 0) == 0 || name.rfind("MENU_MAP_", 0) == 0) &&
        g_icon_imgs.size() < 512)
        g_icon_imgs.push_back({img, x0, y0, x1, y1, w, h, name});
    // Accumulate the UNIQUE name→rect table (skip KG_ button glyphs). Uncapped (dedup set) so
    // it captures whatever menu is open — worldmap (SB_ERR_*) OR inventory/equipment item icons.
    if (!name.empty() && name.rfind("KG_", 0) != 0)
    {
        static std::set<std::string> seen;
        if (seen.insert(name).second)
        {
            spdlog::info("[ICONMAP] '{}' rect=({},{})-({},{}) sheet={}x{} ({}x{})",
                         name, x0, y0, x1, y1, w, h, x1 - x0, y1 - y0);
            // MENU_FL_<N> = a drawn item icon → N is a live iconId anchor. Accumulate + correlate
            // against the EquipParam tables to pin the iconId offset per param ([ICONFIND]).
            if (name.rfind("MENU_FL_", 0) == 0 && name.size() > 8 && name[8] >= '0' && name[8] <= '9')
            {
                uint16_t N = static_cast<uint16_t>(std::atoi(name.c_str() + 8));
                if (std::find(g_menu_iconids.begin(), g_menu_iconids.end(), N) == g_menu_iconids.end())
                {
                    g_menu_iconids.push_back(N);
                    correlate_menu_iconids();   // every new drawn-item iconId → re-correlate
                }
            }
        }
    }
    // Verbose per-image line, capped (avoid flooding when a busy menu creates hundreds).
    if (logged < 60)
    {
        logged++;
        spdlog::info("[ICONTEX] img={:#x} name='{}' rect=({},{})-({},{}) sheet={}x{}",
                     img, name, x0, y0, x1, y1, w, h);
    }
}

void *__fastcall create_image_detour(void *a0, void *a1, void *a2, void *a3, void *a4, void *a5,
                                     void *a6, void *a7)
{
    void *ret = g_create_image_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    icon_log_image((uintptr_t)ret, (uintptr_t)a1, (uintptr_t)a2, (uintptr_t)a3);
    // §5g: capture the live creator context (a0/a1) + log the real symbol name (a2 = param_3 →
    // ASCII @ (*a2 & ~3)+0xc) so we can replay CreateImage for arbitrary item-icon symbols.
    if (g_ci_p1.load(std::memory_order_relaxed) == nullptr) { g_ci_p1.store(a0); g_ci_p2.store(a1); }
    int n = g_ci_logn.fetch_add(1, std::memory_order_relaxed);
    if (n < 60 && a2)
    {
        uintptr_t descv = 0;
        if (icon_rpm_ptr((uintptr_t)a2, descv))
        {
            char nm[96] = {0}; SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), (void *)((descv & ~uintptr_t(3)) + 0xc), nm, 95, &got);
            if (nm[0]) strncpy(g_ci_last, nm, sizeof(g_ci_last) - 1);   // remember for replay control
            spdlog::info("[CREATEIMG] live a0={} a1={} *a2={:#x} name='{}' -> img={:#x}",
                         a0, a1, descv, nm, (uintptr_t)ret);
        }
    }
    return ret;
}

// ── Loaded-image ENUMERATE (sprite findings §1: FUN_140d69640 walks the loaded movie's image
// list [movie+0x40]+0x90, entries type +0x88==4) — the SAFE harvest path (only resident images,
// vs find-by-name which crashes on non-resident). Hook captures the movie ptr (arg0) on the engine
// thread; a one-time dump reverses the entry layout so we can then harvest name+rect+resource.
using enum_fn = void *(__fastcall *)(void *, void *, void *, void *);
enum_fn g_enum_orig = nullptr;
uintptr_t g_inv_movie = 0;

void harvest_resident_icons(uintptr_t movie);   // §8 walk-all (defined after find_detour/g_find_orig)
void harvest_repo_icons();                       // §8b repo-tree walk-all (RE-validated, see below)
void harvest_twin_map_icons(uintptr_t repo, uintptr_t er);  // §8c twin map repo+0xb0 (pin/grace sprites)
void cache_map_sprite_from_img(const char *nm, uintptr_t img);

// Map-point icon layout, resolved from the RESIDENT image repo (the RAM source — see the §8c twin
// walk / cache_map_sprite_from_img). MENU_MAP_<NN> → iconId NN (= WORLD_MAP_POINT_PARAM.iconId, RE
// windows_map_point_icon_layout_re_findings.md); MENU_MAP_ERR_*/Church/… stay name-keyed. rect =
// (x,y)+(w,h) on `sheet` (the ID3D12Resource); err = the SB_MapCursor_ERR sheet.
struct MapIconRect { int x, y, w, h; bool err; void *sheet; };
std::map<int, MapIconRect> g_map_icon_rects;          // iconId -> rect
std::map<std::string, MapIconRect> g_map_icon_named;  // full name -> rect
std::mutex g_map_icon_mtx;
void store_map_icon_rect(const char *nm, int x, int y, int w, int h, void *sheet)
{
    if (!nm || w <= 0 || h <= 0) return;
    MapIconRect r{x, y, w, h, std::strncmp(nm, "MENU_MAP_ERR", 12) == 0, sheet};
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    g_map_icon_named[nm] = r;
    // The GFx import/resolve formats "<symbol>_ptl", so the resident name often carries that suffix
    // (MENU_MAP_ERR_Boss_ptl) while callers look up the bare symbol (category_gpu_icon_name =
    // "MENU_MAP_ERR_Boss"). Register the bare name TOO so the exact lookup hits — this was the gate
    // hiding the boss/world map symbols.
    {
        size_t n = std::strlen(nm);
        if (n > 4 && std::strcmp(nm + n - 4, "_ptl") == 0)
            g_map_icon_named[std::string(nm, n - 4)] = r;
    }
    if (std::strncmp(nm, "MENU_MAP_", 9) == 0)
    {
        const char *p = nm + 9;                 // MENU_MAP_<NN> → iconId NN
        if (*p >= '0' && *p <= '9')
        {
            int iid = std::atoi(p);
            bool fresh = g_map_icon_rects.find(iid) == g_map_icon_rects.end();
            g_map_icon_rects[iid] = r;
            // Enumerate available map-point symbols so the category_gpu_iconId table can be
            // filled from real data (which iconId = which symbol). One line per new iconId.
            if (fresh && goblin::config::dumpIconTextures)
                spdlog::info("[MAPICON-AVAIL] iconId={} name={} rect=({},{} {}x{})", iid, nm, x, y, w, h);
        }
    }
}

void *__fastcall enum_detour(void *movie, void *a1, void *a2, void *a3)
{
    g_inv_movie = reinterpret_cast<uintptr_t>(movie);
    void *r = g_enum_orig(movie, a1, a2, a3);
    // Proactive harvest: walk THIS movie's resident image-list (sprite findings §8) instead of
    // waiting for the find hook to catch each icon as the player browses. enum_detour fires for
    // MANY movies (load screens etc.) — we must walk the CURRENT arg, not a stashed global, so the
    // inventory movie (many MENU_ItemIcon) actually gets processed. Engine thread, post-original.
    harvest_resident_icons(reinterpret_cast<uintptr_t>(movie));
    return r;
}

// ── HARVEST via the find hook (the SAFE path): the engine calls FUN_140d63c30(repo,&out,key) to
// resolve each LOADED image (during enumeration + menu draws) — so hooking it captures every
// RESIDENT item icon's name+rect+sheet, on the engine thread, no container-reversing, no risky
// self-initiated find (the engine only finds what's loaded → never the crashing non-resident path).
using find_fn2 = void *(__fastcall *)(void *, void **, const wchar_t *);
find_fn2 g_find_orig = nullptr;

// Harvested icon cache: iconId → {sheet ID3D12Resource, sub-rect, format}. Engine thread (find
// hook) writes; render thread (overlay) reads → mutex. The render thread CopyTextureRegions from
// these into our own SRV atlas.
std::mutex g_harvest_mtx;
std::unordered_map<int, goblin::ItemSprite> g_harvest;
uintptr_t g_icon_repo = 0;   // FUN_140d63c30 arg0 (repo) — stashed for the proactive §8 walk
// Discovered/lit grace sprite (RE e4b3f6a §6: SB_ERR_Grace_Morning_Color, world-map movie). Harvested
// by the SAME find hook when the open map draws a discovered grace → lets the overlay draw graces
// itself (discovered = this sprite, undiscovered = grey-tinted) and become the sole grace source.
goblin::ItemSprite g_grace_sprite{};
bool g_grace_locked = false;   // true once the canonical grace (MENU_MAP_01_Bonfire) is stored
std::vector<goblin::GraceCandidate> g_grace_cands;   // all grace candidates (dev F1 viewer)
// ERR dungeon-style grace (MENU_MAP_ERR_GraceUnderground). Valid iff ERR is installed (the sprite
// exists in the worldmap gfx). The renderer uses it for DUNGEON graces in place of the vanilla bonfire.
goblin::ItemSprite g_grace_dungeon_sprite{};

// Read a resolved CSTextureImage (`img` = the find fn's `out`) and cache its sub-rect + backing
// sheet resource + DXGI_FORMAT under the iconId parsed from a MENU_ItemIcon_<id> name. Shared by
// the find hook (browse-to-fill) and the §8 proactive walk. Read-only (RPM). Returns true if cached.
bool cache_icon_from_img(const char *nm, uintptr_t img)
{
    if (img < 0x10000 || std::strncmp(nm, "MENU_ItemIcon_", 14) != 0) return false;
    uintptr_t vt = 0; icon_rpm_ptr(img, vt);
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!(vt > er && vt - er == 0x2bb8910)) return false;   // CSTextureImage vtable guard
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    uintptr_t rtex = 0, res = 0;
    icon_rpm_ptr(img + 0x10, rtex);
    if (rtex > 0x10000) icon_rpm_ptr(rtex + 0x70, res);
    if (res <= 0x10000) return false;
    int iconId = std::atoi(nm + 14);
    // Sheet dims/format live on the bound D3D12 resource, but the GPU texture binds LAZILY
    // (findings §2) — at find/walk time res+0x10/+0x30 may still be 0. Do NOT gate harvesting on
    // that (it dropped every not-yet-rendered icon → harvested=0). Cache the rect + sheet ptr now
    // (the ID3D12Resource* is persistent); the overlay reads the AUTHORITATIVE format/dims via
    // ID3D12Resource::GetDesc() at copy time (render thread, bound). RPM read here is best-effort
    // diagnostics only (correct once bound, else 0).
    int dim = 0, rw = 0, rh = 0, fmt = 0;
    icon_rpm_i32(res + 0x10, dim); icon_rpm_i32(res + 0x20, rw); icon_rpm_i32(res + 0x28, rh);
    if (dim == 3) icon_rpm_i32(res + 0x30, fmt);
    goblin::ItemSprite hs;
    hs.sheet = reinterpret_cast<void *>(res);
    hs.x0 = x0; hs.y0 = y0; hs.x1 = x1; hs.y1 = y1;
    hs.sheetW = static_cast<unsigned long long>(rw);
    hs.sheetH = static_cast<unsigned>(rh);
    hs.format = static_cast<unsigned>(fmt);
    hs.valid = true;
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    g_harvest[iconId] = hs;
    return true;
}

void *__fastcall find_detour(void *repo, void **out, const wchar_t *key)
{
    if (repo && !g_icon_repo) g_icon_repo = reinterpret_cast<uintptr_t>(repo);
    void *ret = g_find_orig(repo, out, key);
    if (goblin::config::dumpIconTextures && key)
    {
        char nm[48] = {0};
        for (int i = 0; i < 47; ++i) { wchar_t c = key[i]; if (!c) break; nm[i] = (c < 128) ? static_cast<char>(c) : '?'; }
        // DIAG: log EVERY resolved sprite key + rect (deduped) → reveals the map-point naming scheme.
        // If the key encodes the WorldMapPointParam iconId (numeric) we get iconId→rect for free; if
        // it's symbolic (MENU_MAP_01_Bonfire) the marker iconId→rect needs a pin-draw hook instead.
        {
            uintptr_t img2 = 0; icon_rpm_ptr(reinterpret_cast<uintptr_t>(out), img2);
            if (img2 < 0x10000) img2 = reinterpret_cast<uintptr_t>(ret);
            uintptr_t vt2 = 0; icon_rpm_ptr(img2, vt2);
            uintptr_t er2 = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
            static std::set<std::string> seen2;
            if (img2 > 0x10000 && vt2 > er2 && vt2 - er2 == 0x2bb8910 && nm[0] &&
                seen2.size() < 400 && seen2.insert(nm).second)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                icon_rpm_i32(img2 + 0x74, x0); icon_rpm_i32(img2 + 0x78, y0);
                icon_rpm_i32(img2 + 0x7c, x1); icon_rpm_i32(img2 + 0x80, y1);
                spdlog::info("[MAPICON] key='{}' rect=({},{})-({},{})", nm, x0, y0, x1, y1);
            }
        }
        if (nm[0] == 'M' && nm[1] == 'E' && nm[2] == 'N' && nm[3] == 'U')   // MENU_* item icons
        {
            uintptr_t img = 0; icon_rpm_ptr(reinterpret_cast<uintptr_t>(out), img);
            if (img < 0x10000) img = reinterpret_cast<uintptr_t>(ret);
            uintptr_t vt = 0; icon_rpm_ptr(img, vt);
            uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
            static std::set<std::string> seen;
            if (img > 0x10000 && vt > er && vt - er == 0x2bb8910 && seen.insert(nm).second)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
                icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
                uintptr_t rtex = 0, res = 0;
                icon_rpm_ptr(img + 0x10, rtex);
                if (rtex > 0x10000) icon_rpm_ptr(rtex + 0x70, res);
                spdlog::info("[ENUM2] '{}' img={:#x} rect=({},{})-({},{}) res={:#x}", nm, img, x0, y0, x1, y1, res);
                // Cache per-iconId for the overlay's CopyTextureRegion (MENU_ItemIcon_<id> only).
                cache_icon_from_img(nm, img);
                // P2b: one-time dump of the sheet resource's first 0x60 bytes (i32) to LOCATE the
                // DXGI_FORMAT field (a small int near W@0x20/H@0x28; e.g. 71=BC1,77=BC3,98=BC7,87=BGRA8).
                static bool fmt_dumped = false;
                if (!fmt_dumped && res > 0x10000)
                {
                    fmt_dumped = true;
                    for (int o = 0; o < 0x60; o += 4)
                    {
                        int fv = 0; icon_rpm_i32(res + o, fv);
                        spdlog::info("[ENUM2-FMT] res+0x{:02x} = {} (0x{:x})", o, fv, static_cast<unsigned>(fv));
                    }
                }
            }
        }
    }
    // §8b repo-tree harvest RELOCATED off this game-thread hook → goblin::background_harvest_tick()
    // on the worldmap_probe background poll (docs/rpm_walk_audit.md). find_detour still fires
    // constantly while a menu is open, so running the RPM walk here put Wine's per-RPM cost on the
    // engine thread; the walk is self-throttled + reads the repo from its static anchor, so the
    // background poll covers it without stalling the engine. We keep stashing g_icon_repo above as
    // the walk's fallback anchor + the dumpIconTextures diag's per-find cache.
    return ret;
}

// §8 proactive harvest (sprite findings §8, commit 6913ec4): walk the loaded movie's resident
// image-list in ONE pass and cache every MENU_ItemIcon_* (vs the find hook, which only catches the
// one icon the engine happens to resolve while the player browses). Layout (instruction-confirmed):
//   movie+0x40 -> res ; gate res+0x88 == 4 (image-list) ; res+0x90 -> list
//   list+0x78 u32 count ; list+0x80 -> arr (entry-ptr array, stride 8)
//   entry+0x18 = name DLWString (heap iff *(entry+0x30) >= 8, else inline) ; names are GFx symbols.
// Each name is RESIDENT by construction → re-resolving it via the find fn (g_find_orig, the original
// trampoline — does NOT re-enter find_detour) is SAFE (the non-resident crash constraint doesn't
// apply). Read-only walk; runs on the engine thread from enum_detour. Throttled (500ms).
void harvest_resident_icons(uintptr_t movie)
{
    if (!goblin::config::dumpIconTextures || movie < 0x10000 || !g_icon_repo || !g_find_orig)
        return;

    uintptr_t res = 0; icon_rpm_ptr(movie + 0x40, res);
    if (res < 0x10000) return;
    uintptr_t list = 0; icon_rpm_ptr(res + 0x90, list);
    if (list < 0x10000) return;
    int count = 0; icon_rpm_i32(list + 0x78, count);
    uintptr_t arr = 0; icon_rpm_ptr(list + 0x80, arr);
    if (count <= 0 || count > 100000 || arr < 0x10000) return;
    // NOTE: the static +0x88==4 type gate from the findings reads garbage live (0x44) → dropped;
    // we instead filter by entry NAME (only MENU_ItemIcon_* are cached) which is self-validating.

    // Process each distinct (movie,count) once — enum_detour fires every frame for many movies; this
    // dedup avoids re-walking + re-resolving an unchanged list. A grown list (new count) reprocesses.
    static std::set<uint64_t> s_done;
    uint64_t key = (static_cast<uint64_t>(movie) * 1000003u) ^ static_cast<uint64_t>(count);
    if (!s_done.insert(key).second) return;

    // Game-thread RPM walk (enum_detour) → keep it bench-visible so a Wine RPM-cost regression on
    // this dev-gated path shows in [BENCH] instead of being an invisible freeze (docs/rpm_walk_audit.md).
    GOBLIN_BENCH_QUIET("harvest.resident_walk");

    int loops = count < 4096 ? count : 4096;   // hard cap (a non-list movie could give a bogus count)
    int harvested = 0, names_menu = 0;
    for (int i = 0; i < loops; ++i)
    {
        uintptr_t entry = 0; icon_rpm_ptr(arr + static_cast<uintptr_t>(i) * 8, entry);
        if (entry < 0x10000) continue;
        uintptr_t len = 0; icon_rpm_ptr(entry + 0x30, len);          // name length (wchars)
        uintptr_t namep = (len >= 8) ? 0 : entry + 0x18;             // SSO inline vs heap
        if (len >= 8) icon_rpm_ptr(entry + 0x18, namep);
        if (namep < 0x1000) continue;

        wchar_t wbuf[80] = {0}; SIZE_T got = 0;                       // one RPM for the whole name
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(namep), wbuf,
                          sizeof(wbuf) - sizeof(wchar_t), &got);
        char nm[80]; int k = 0;
        for (; k < 79 && wbuf[k]; ++k) nm[k] = (wbuf[k] < 128) ? static_cast<char>(wbuf[k]) : '?';
        nm[k] = 0;
        if (std::strncmp(nm, "MENU_ItemIcon_", 14) != 0) continue;
        ++names_menu;
        int iconId = std::atoi(nm + 14);
        { std::lock_guard<std::mutex> lk(g_harvest_mtx); if (g_harvest.count(iconId)) continue; }

        // Resolve the resident image by name (safe; trampoline, not the detour) → read+cache.
        // The fn writes the image into *out; some paths return it instead.
        void *out = nullptr;
        void *ret = g_find_orig(reinterpret_cast<void *>(g_icon_repo), &out, wbuf);
        uintptr_t img = reinterpret_cast<uintptr_t>(out);
        if (img < 0x10000) img = reinterpret_cast<uintptr_t>(ret);
        if (cache_icon_from_img(nm, img)) ++harvested;
    }
    if (names_menu || harvested)
        spdlog::info("[ENUM-WALK] §8 movie={:#x} count={} MENU_ItemIcon_names={} harvested_new={}",
                     movie, count, names_menu, harvested);
}

// §8b PROACTIVE repo-walk harvest — SOLVED + validated live (163 icons / 938 images, 2026-06-22).
// See docs/re/windows_resident_icon_enumeration_re_findings.md. Walk the FD4 image-repo's by-name
// std::map (a red-black tree) directly and cache EVERY resident MENU_ItemIcon_* in one pass —
// movie-independent (the §8 movie-walk only saw load-screen movies on this build), reading the
// CSTextureImage* straight from node+0x50 (NO find-by-name re-resolve, so the §5 non-resident crash
// can't occur). Read-only RPM, runs on the engine thread from find_detour; reprocesses only when the
// resident image count (_Mysize) grows, so the steady-state cost is two RPM reads + a compare.
//   repo    = *(er+0x3d82510)  (DAT_143d82510; fallback to g_icon_repo stashed by find_detour)
//   _Myhead = *(repo+0x88) ; root = *(_Myhead+0x08) ; _Mysize = *(repo+0x90)
//   node: _Left+0x00 _Parent+0x08 _Right+0x10 _Isnil(u8)+0x19 ; key DLWString+0x28 (len+0x38,cap+0x40)
//         value = CS::CSTextureImage* @ node+0x50
void harvest_repo_icons()
{
    // Run whenever native icons are wanted — graces (grace_overlay) and item icons
    // (native_item_icons) BOTH consume the harvested rects/sprites this walk caches, plus the dev
    // dump flag. Previously gated dump-only, which forced users onto the laggy debug flag to get
    // GPU graces. The walk below bulk-reads each node (1 RPM) + is rate-throttled so Wine's
    // per-RPM cost doesn't stall on icon churn (the old per-field reads were the Linux freeze).
    if (!goblin::config::dumpIconTextures && !goblin::config::graceOverlay &&
        !goblin::config::nativeItemIcons)
        return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return;

    uintptr_t repo = 0; icon_rpm_ptr(er + 0x3d82510, repo);  // the canonical static anchor
    if (repo < 0x10000) repo = g_icon_repo;                  // fallback: arg0 stashed by find_detour
    if (repo < 0x10000) return;

    uintptr_t head = 0; icon_rpm_ptr(repo + 0x88, head);     // _Myhead (nil sentinel == end())
    if (head < 0x10000) return;
    int size = 0; icon_rpm_i32(repo + 0x90, size);           // _Mysize (count of ALL resident images)

    // Re-walk on ANY change to the resident count, not just growth. _Mysize OSCILLATES as the engine
    // evicts+loads icons while navigating menus (live monitor: 1074<->1107 churn), so the old
    // "walk only when it grows past the peak" gate stopped re-walking after the first peak and missed
    // every icon that loaded below it. Walking on each change + the per-iconId dedup (g_harvest.count
    // below) accumulates the UNION of everything that ever loads into our permanent cache → far wider
    // browse-to-fill coverage. Cheap: only fires when the count actually changed, not every frame.
    static int s_last_size = -1;
    static DWORD s_last_walk = 0;
    if (size == s_last_size) return;
    // Wine RPM is ~expensive: in production (no dev dump) cap the re-walk rate so the _Mysize churn
    // (icons evict/load while navigating, e.g. 1074<->1107) can't trigger a walk-storm freeze. The
    // map symbols/graces we need load once and persist in our cache, so an occasional walk suffices.
    // (Skip without updating s_last_size so the next post-throttle change still triggers a walk.)
    if (!goblin::config::dumpIconTextures && (GetTickCount() - s_last_walk) < 400)
        return;
    s_last_size = size;
    s_last_walk = GetTickCount();
    // BENCH (quiet = aggregate only, no per-call log spam): this runs on the GAME thread (find_detour),
    // which the per-frame render bench never sees — so a future Linux RPM-walk regression shows up in
    // the [BENCH] report instead of being an invisible freeze. See [[overlay-rendered-markers]] #11.
    GOBLIN_BENCH_QUIET("harvest.repo_walk");

    uintptr_t root = 0; icon_rpm_ptr(head + 0x08, root);     // _Myhead._Parent
    if (root < 0x10000) return;

    // Stack DFS over _Left/_Right, skipping nil sentinels (_Isnil@+0x19 != 0). Tree height is
    // O(log n) so the frontier stays small; the vector grows if a build ever holds a huge repo.
    std::vector<uintptr_t> stack; stack.reserve(64); stack.push_back(root);
    int walked = 0, harvested = 0;
    while (!stack.empty())
    {
        if (walked > 200000) break;                          // runaway guard
        uintptr_t n = stack.back(); stack.pop_back();
        if (n < 0x10000) continue;
        // BULK-READ the node header in ONE RPM (was ~6 per-field reads/node = thousands of RPM/walk
        // = the Wine freeze). Layout: _Left@0x00 _Right@0x10 _Isnil@0x19 key-DLWString@0x28
        // (inline chars, or heap ptr iff cap>=8) cap@0x40 value(CSTextureImage*)@0x50.
        uint8_t nb[0x58]; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(n), nb, sizeof(nb), &got) ||
            got != sizeof(nb) || nb[0x19])
            continue;                                        // read fail or nil sentinel
        ++walked;
        const uintptr_t l = *reinterpret_cast<uintptr_t *>(nb + 0x00);
        const uintptr_t r = *reinterpret_cast<uintptr_t *>(nb + 0x10);
        const uint64_t cap = *reinterpret_cast<uint64_t *>(nb + 0x40);
        const uintptr_t img = *reinterpret_cast<uintptr_t *>(nb + 0x50);

        char nm[80] = {0};
        if (cap < 8)                                         // inline name (rare for MENU_* — they're long)
        {
            const wchar_t *w = reinterpret_cast<const wchar_t *>(nb + 0x28);
            for (int k = 0; k < 7 && w[k]; ++k) nm[k] = (w[k] < 128) ? static_cast<char>(w[k]) : '?';
        }
        else                                                 // heap name → one extra RPM
        {
            uintptr_t namep = *reinterpret_cast<uintptr_t *>(nb + 0x28);
            if (namep >= 0x1000)
            {
                wchar_t wbuf[80] = {0}; SIZE_T g2 = 0;
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(namep), wbuf,
                                  sizeof(wbuf) - sizeof(wchar_t), &g2);
                for (int k = 0; k < 79 && wbuf[k]; ++k)
                    nm[k] = (wbuf[k] < 128) ? static_cast<char>(wbuf[k]) : '?';
            }
        }
        if (nm[0])
        {
            if (std::strncmp(nm, "MENU_ItemIcon_", 14) == 0)
            {
                int iconId = std::atoi(nm + 14);
                bool have;
                { std::lock_guard<std::mutex> lk(g_harvest_mtx); have = g_harvest.count(iconId) != 0; }
                if (!have && cache_icon_from_img(nm, img)) ++harvested;   // value = CSTextureImage*
            }
            // Map-point icons (MENU_MAP_<NN>/_ERR_*) live in THIS by-name tree (repo+0x80), not the
            // +0xb0 twin (which holds MENU_MapTile_*). Read their rect @img+0x74..0x80 + backing sheet
            // and cache iconId->rect for the overlay (RE windows_map_point_icon_layout_re_findings.md).
            else if (std::strncmp(nm, "MENU_MAP_", 9) == 0 && img >= 0x10000)
            {
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
                icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
                uintptr_t rtex = 0, res = 0;
                icon_rpm_ptr(img + 0x10, rtex);
                if (rtex >= 0x10000) icon_rpm_ptr(rtex + 0x70, res);
                if (x1 - x0 >= 2 && y1 - y0 >= 2 && res >= 0x10000)
                    store_map_icon_rect(nm, x0, y0, x1 - x0, y1 - y0, reinterpret_cast<void *>(res));
                // The overworld grace (MENU_MAP_01_Bonfire) + the ERR dungeon grace
                // (MENU_MAP_ERR_GraceUnderground) are the sprites the overlay draws. run_force_grace's
                // CreateImage lands them in THIS repo+0x80 tree, so route them to cache_map_sprite_from_img
                // (sets g_grace_sprite / g_grace_dungeon_sprite). NOT the SB_ERR_Grace_*_Color time-of-day
                // pin variant (that gave the wrong "morning grace"). Map symbols (boss etc) use the rect above.
                if (std::strcmp(nm, "MENU_MAP_01_Bonfire") == 0 ||
                    std::strcmp(nm, "MENU_MAP_ERR_GraceUnderground") == 0)
                    cache_map_sprite_from_img(nm, img);
            }
        }

        if (l >= 0x10000) stack.push_back(l);
        if (r >= 0x10000) stack.push_back(r);
    }
    if (harvested && goblin::config::dumpIconTextures)
        spdlog::info("[REPO-WALK] §8b images={} walked={} harvested_new={}", size, walked, harvested);

    harvest_twin_map_icons(repo, er);
}

// Build an ItemSprite from a CSTextureImage* (rect + backing sheet) and register it as a grace
// CANDIDATE — used by the twin-map walk for the WorldMapPoint pin sprites (MENU_MAP_*). Mirrors
// cache_icon_from_img but keyed by NAME (not iconId): rect @img+0x74.., sheet = rtex+0x70 (no DIM
// gate — GetDesc resolves it at copy time). If the name is the canonical grace, set g_grace_sprite.
void cache_map_sprite_from_img(const char *nm, uintptr_t img)
{
    if (img < 0x10000) return;
    uintptr_t vt = 0; icon_rpm_ptr(img, vt);
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!(vt > er && vt - er == 0x2bb8910)) return;     // CSTextureImage vtable guard
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    icon_rpm_i32(img + 0x74, x0); icon_rpm_i32(img + 0x78, y0);
    icon_rpm_i32(img + 0x7c, x1); icon_rpm_i32(img + 0x80, y1);
    uintptr_t rtex = 0, res = 0;
    icon_rpm_ptr(img + 0x10, rtex);
    if (rtex > 0x10000) icon_rpm_ptr(rtex + 0x70, res);
    if (res <= 0x10000) return;
    if (x1 - x0 < 2 || y1 - y0 < 2) return;             // need a real rect
    int rw = 0, rh = 0, fmt = 0;
    icon_rpm_i32(res + 0x20, rw); icon_rpm_i32(res + 0x28, rh); icon_rpm_i32(res + 0x30, fmt);
    goblin::ItemSprite hs;
    hs.sheet = reinterpret_cast<void *>(res);
    hs.x0 = x0; hs.y0 = y0; hs.x1 = x1; hs.y1 = y1;
    hs.sheetW = static_cast<unsigned long long>(rw);
    hs.sheetH = static_cast<unsigned>(rh);
    hs.format = static_cast<unsigned>(fmt);
    hs.valid = true;

    // Map-point icon layout from the RESIDENT repo (the RAM source — no sblytbnd decompress needed):
    // iconId/name -> rect (x0,y0,x1,y1 = left,top,right,bottom) + the backing sheet. Drives the overlay
    // MapPointProvider. RE windows_map_point_icon_layout_re_findings.md (§ repo walk).
    store_map_icon_rect(nm, x0, y0, x1 - x0, y1 - y0, reinterpret_cast<void *>(res));

    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    bool dup = false;
    for (const auto &gc : g_grace_cands) if (gc.name == nm) { dup = true; break; }
    if (!dup && g_grace_cands.size() < 64)
    {
        goblin::GraceCandidate gc; gc.name = nm; gc.spr = hs;
        g_grace_cands.push_back(gc);
        spdlog::info("[TWIN-WALK] candidate '{}' rect=({},{})-({},{}) res={:#x} fmt={}",
                     nm, x0, y0, x1, y1, res, fmt);
    }
    // Overworld grace = the vanilla bonfire icon MENU_MAP_01_Bonfire (the mod's MENU_MAP_GOBLIN_Grace
    // doesn't exist → CreateImage returned 0). Dungeon grace = MENU_MAP_ERR_GraceUnderground. NOT the
    // SB_ERR_Grace_*_Color pin (time-of-day variant = the wrong "morning grace"); no generic fallback.
    if (std::strcmp(nm, "MENU_MAP_01_Bonfire") == 0)
    {
        g_grace_sprite = hs; g_grace_locked = true;
        spdlog::info("[GRACE-SPRITE] '{}' LOCKED overworld bonfire rect=({},{})-({},{}) res={:#x}",
                     nm, x0, y0, x1, y1, res);
    }
    else if (std::strcmp(nm, "MENU_MAP_ERR_GraceUnderground") == 0 && !g_grace_dungeon_sprite.valid)
    {
        g_grace_dungeon_sprite = hs;
        spdlog::info("[GRACE-SPRITE] '{}' dungeon rect=({},{})-({},{}) res={:#x}",
                     nm, x0, y0, x1, y1, res);
    }
}

// §8c TWIN-map walk — the WorldMapPoint PIN icons (the REAL grace) live in the repo's TWIN std::map
// at repo+0xb0 (head +0xb8, size +0xc0), keyed by the gfx sprite name (MENU_MAP_*), looked up by the
// icon widget FUN_14074bcc0 via FUN_140d63e50 — NOT in repo+0x80 (MENU_ItemIcon). We never walked it,
// which is why we only ever found the wrong SB_ERR_Grace native pin. Same RB-tree node shape as §8b.
void harvest_twin_map_icons(uintptr_t repo, uintptr_t er)
{
    (void)er;
    if (repo < 0x10000) return;
    uintptr_t head = 0; icon_rpm_ptr(repo + 0xb8, head);
    if (head < 0x10000) return;
    int size = 0; icon_rpm_i32(repo + 0xc0, size);
    static int s_last_twin = -1;
    static DWORD s_last_twin_walk = 0;
    if (size == s_last_twin) return;                    // re-walk only when the twin count changes
    if (!goblin::config::dumpIconTextures && (GetTickCount() - s_last_twin_walk) < 400)
        return;                                         // prod: cap re-walk rate (Wine RPM cost)
    s_last_twin = size;
    s_last_twin_walk = GetTickCount();
    GOBLIN_BENCH_QUIET("harvest.twin_walk");            // game-thread RPM walk → keep it bench-visible
    uintptr_t root = 0; icon_rpm_ptr(head + 0x08, root);
    if (root < 0x10000) return;

    std::vector<uintptr_t> stack; stack.reserve(64); stack.push_back(root);
    int walked = 0, found = 0;
    while (!stack.empty())
    {
        if (walked > 200000) break;
        uintptr_t n = stack.back(); stack.pop_back();
        if (n < 0x10000) continue;
        // BULK-READ the node header in ONE RPM (was ~6 per-field reads = the Wine freeze). Same RB-tree
        // node layout as the repo walk: _Left@0x00 _Right@0x10 _Isnil@0x19 key@0x28 cap@0x40 value@0x50.
        uint8_t nb[0x58]; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(n), nb, sizeof(nb), &got) ||
            got != sizeof(nb) || nb[0x19])
            continue;
        ++walked;
        const uintptr_t l = *reinterpret_cast<uintptr_t *>(nb + 0x00);
        const uintptr_t r = *reinterpret_cast<uintptr_t *>(nb + 0x10);
        const uint64_t cap = *reinterpret_cast<uint64_t *>(nb + 0x40);
        const uintptr_t img = *reinterpret_cast<uintptr_t *>(nb + 0x50);
        char nm[96] = {0};
        if (cap < 8)
        {
            const wchar_t *w = reinterpret_cast<const wchar_t *>(nb + 0x28);
            for (int k = 0; k < 7 && w[k]; ++k) nm[k] = (w[k] < 128) ? static_cast<char>(w[k]) : '?';
        }
        else
        {
            uintptr_t namep = *reinterpret_cast<uintptr_t *>(nb + 0x28);
            if (namep >= 0x1000)
            {
                wchar_t wbuf[96] = {0}; SIZE_T g2 = 0;
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(namep), wbuf,
                                  sizeof(wbuf) - sizeof(wchar_t), &g2);
                for (int k = 0; k < 95 && wbuf[k]; ++k)
                    nm[k] = (wbuf[k] < 128) ? static_cast<char>(wbuf[k]) : '?';
            }
        }
        if (nm[0])
        {
            ++found;
            // Capture map-point sprites (MENU_MAP_*) and anything grace-ish into grace candidates.
            if (std::strncmp(nm, "MENU_MAP_", 9) == 0 || std::strstr(nm, "Grace"))
                cache_map_sprite_from_img(nm, img);
        }
        if (l >= 0x10000) stack.push_back(l);
        if (r >= 0x10000) stack.push_back(r);
    }
    if (goblin::config::dumpIconTextures)
        spdlog::info("[TWIN-WALK] repo+0xb0 size={} walked={} named={}", size, walked, found);
}
} // namespace

// Global-scope forward decl (definition is at file scope, ~g_dds_list) so goblin::force_load_file
// can scan a force-loaded sblytbnd's resident BND4 for the item-icon layout.
void parse_item_icon_layout(const uint8_t *buf, size_t n);

// Verify item↔iconId↔sprite: iterate the EquipParam* tables LIVE (get_param), read each row's
// iconId at its paramdef offset (u16), and log the rows whose iconId matches the sprites we
// captured from the inventory (MENU_FL_<iconId>). Proves the menu icon = EquipParam.iconId.
// iconId offsets (Ghidra+1, live-corrected): Weapon 0xC0, Protector iconIdM 0xA8,
// Accessory 0x28, Goods 0x32, Gem 0x06. row_id == the item-name FMG id.
void goblin::verify_equip_iconids()
{
    if (!goblin::config::dumpIconTextures)
        return;
    struct P { const wchar_t *name; const char *tag; int off; };
    // iconId offsets (u16) — SOLVED + cross-verified (2026-06-22): computed from the CURRENT
    // (SOTE) Paramdex (Nordgaren/Erd-Tools Defs-English) by tools/paramdef_iconid_offset.py AND
    // confirmed against the live [CALIB] high-distinct columns. The earlier 0xC0 "confirmation" was
    // DURABILITY (paramdef order: equipModelId@0xBC, iconId@0xBE, durability@0xC0; Dagger has both
    // iconId=100 AND durability=100 → coincidence). distinct=2 @ 0xC0 = durability default 100.
    //   Weapon iconId=0xBE (live distinct=599 range[20,45818]); Protector iconIdM=0xA6 / iconIdF=0xA8
    //   (distinct=760 range[1097,45808]); Accessory=0x26 (distinct=167/170); Goods=0x30
    //   (distinct=1573, contains the live-captured 40144); Gem=0x04 (distinct=609).
    static const P params[] = {
        {L"EquipParamWeapon", "Weapon", 0xBE},
        {L"EquipParamProtector", "Protector", 0xA6}, // iconIdM (iconIdF = 0xA8)
        {L"EquipParamAccessory", "Accessory", 0x26},
        {L"EquipParamGoods", "Goods", 0x30},
        {L"EquipParamGem", "Gem", 0x04},
    };
    static const uint16_t want[] = {40144, 40147, 40172}; // the 3 inventory-captured iconIds
    for (const P &p : params)
    {
        try
        {
            int n = 0, sample = 0; uint32_t mn = 0xffffffff, mx = 0; int hits = 0;
            uintptr_t prev_base = 0;
            for (auto row : from::params::get_param<uint8_t>(p.name))
            {
                uintptr_t base = reinterpret_cast<uintptr_t>(&row.second);
                uint16_t icon = *reinterpret_cast<uint16_t *>(base + p.off);
                ++n;
                if (icon < mn) mn = icon;
                if (icon > mx) mx = icon;
                if (sample < 8 && icon != 0)   // skip unused/system rows (iconId 0) so the sample
                                               // shows REAL iconIds at the new offset
                {
                    // [EQUIPADDR] = the absolute row-base + name + struct stride, for CE
                    // find-what-accesses: watch <base + iconId-offset> and trigger the menu
                    // icon draw to capture the compiled `movzx r,[reg+0xXX]` (XX = true offset).
                    std::string nm = goblin::lookup_text_utf8(static_cast<int>(row.first));
                    uintptr_t stride = prev_base ? (base - prev_base) : 0;
                    spdlog::info("[EQUIPADDR] {} row_id={} base={:#x} stride={:#x} u16@0x{:x}={} name='{}'",
                                 p.tag, row.first, base, stride, p.off, icon, nm);
                    ++sample;
                }
                prev_base = base;
                for (uint16_t w : want)
                    if (icon == w)
                    {
                        std::string nm = goblin::lookup_text_utf8(static_cast<int>(row.first));
                        spdlog::info("[EQUIPVERIFY] {} MATCH row_id={} iconId={} name='{}'", p.tag, row.first, icon, nm);
                        ++hits;
                    }
            }
            spdlog::info("[EQUIPVERIFY] {} rows={} iconId[min={},max={}] hits={}", p.tag, n, mn, mx, hits);
        }
        catch (...)
        {
            spdlog::info("[EQUIPVERIFY] {} not loaded", p.tag);
        }
    }
}

// Self-calibrating iconId-offset finder (RE §3, windows_live_paramdef_offset_re_findings).
// For each EquipParam, scan every u16 column across all rows and rank the offsets whose
// distribution looks like an iconId (many distinct, plausible range, ideally containing a
// known anchor). Zero offline / zero hardcoded offset / survives ER patches + mod swaps.
// Anchors: Weapon row 1000 (Dagger) = iconId 100; the live-captured inventory iconIds
// {40144,40147,40172} ([ICONMAP]) tag whichever param actually owns those items.
static void self_calibrate_iconid()
{
    if (!goblin::config::dumpIconTextures)
        return;
    struct P { const wchar_t *name; const char *tag; int known_off; };
    static const P params[] = {
        {L"EquipParamWeapon", "Weapon", 0xC0},
        {L"EquipParamProtector", "Protector", -1},
        {L"EquipParamAccessory", "Accessory", -1},
        {L"EquipParamGoods", "Goods", -1},
        {L"EquipParamGem", "Gem", -1},
    };
    static const uint16_t anchors[] = {40144, 40147, 40172};
    for (const P &p : params)
    {
        try
        {
            std::vector<uintptr_t> bases;
            for (auto row : from::params::get_param<uint8_t>(p.name))
                bases.push_back(reinterpret_cast<uintptr_t>(&row.second));
            if (bases.size() < 2)
            {
                spdlog::info("[CALIB] {} too few rows", p.tag);
                continue;
            }
            uintptr_t stride = bases[1] - bases[0];
            if (stride < 4 || stride > 0x2000)
            {
                spdlog::info("[CALIB] {} non-contiguous rows (stride={:#x}) — skip", p.tag, stride);
                continue;
            }
            int N = static_cast<int>(bases.size());
            // iconId is LOW-cardinality (icons reused across many items) but DENSE in a small
            // low band — unlike id/price columns (huge sparse range). Rank by density =
            // distinct/span; that's what separates iconId from the high-distinct id columns.
            struct Cand { int off, distinct, validPct, density; uint16_t mn, mx; bool anchor; };
            std::vector<Cand> cands;
            for (uintptr_t off = 0; off + 2 <= stride; ++off)
            {
                std::set<uint16_t> seen;
                int valid = 0; uint16_t mn = 0xffff, mx = 0; bool anc = false;
                for (uintptr_t b : bases)
                {
                    uint16_t v = *reinterpret_cast<uint16_t *>(b + off);
                    if (v >= 1 && v <= 65534) { ++valid; if (v < mn) mn = v; if (v > mx) mx = v; seen.insert(v); }
                    for (uint16_t a : anchors) if (v == a) anc = true;
                }
                int distinct = static_cast<int>(seen.size());
                int span = (mx >= mn) ? (mx - mn + 1) : 1;
                int density = distinct * 1000 / span;            // ×1000 to keep ints meaningful
                if (distinct < 20 || mx > 60000) continue;        // iconId: varied + bounded
                cands.push_back({(int)off, distinct, valid * 100 / N, density, mn, mx, anc});
            }
            std::sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) {
                if (a.anchor != b.anchor) return a.anchor;        // a captured iconId here = strong
                return a.density > b.density;                      // else densest low-band column
            });
            spdlog::info("[CALIB] {} N={} stride={:#x} knownOff=0x{:x} — ranked by density:",
                         p.tag, N, stride, p.known_off);
            for (int i = 0; i < (int)cands.size() && i < 6; ++i)
            {
                const Cand &c = cands[i];
                spdlog::info("[CALIB]   off=0x{:x} density={} distinct={} valid={}% range[{},{}]{}{}",
                             c.off, c.density, c.distinct, c.validPct, c.mn, c.mx,
                             c.anchor ? " <ANCHOR>" : "", c.off == p.known_off ? " <KNOWN>" : "");
            }
            // Also dump the KNOWN offset's stats verbatim (even if it didn't make top-6) so the
            // density heuristic can be calibrated against ground truth (Weapon 0xC0).
            if (p.known_off >= 0)
            {
                std::set<uint16_t> seen; uint16_t mn = 0xffff, mx = 0;
                for (uintptr_t b : bases)
                {
                    uint16_t v = *reinterpret_cast<uint16_t *>(b + p.known_off);
                    if (v >= 1 && v <= 65534) { if (v < mn) mn = v; if (v > mx) mx = v; seen.insert(v); }
                }
                int span = (mx >= mn) ? (mx - mn + 1) : 1;
                spdlog::info("[CALIB]   KNOWN off=0x{:x} density={} distinct={} range[{},{}]",
                             p.known_off, (int)seen.size() * 1000 / span, (int)seen.size(), mn, mx);
            }
        }
        catch (...) { spdlog::info("[CALIB] {} not loaded", p.tag); }
    }
}

goblin::ItemSprite goblin::resolve_item_sprite(int iconId)
{
    // Cache VALID resolves only (a miss may be "sheet not loaded yet" → retry later).
    static std::unordered_map<int, ItemSprite> cache;
    auto it = cache.find(iconId);
    if (it != cache.end()) return it->second;

    ItemSprite s;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return s;
    using FindFn = void *(__fastcall *)(void *, void **, const wchar_t *);  // FUN_140d63c30
    auto find = reinterpret_cast<FindFn>(er + 0xd63c30);
    void *repo = *reinterpret_cast<void **>(er + 0x3d82510);  // FD4 image repo singleton
    if (!repo) return s;                                       // menu not loaded → no sheets resident
    wchar_t key[40] = L"MENU_ItemIcon_00000";
    { int v = iconId; for (int i = 18; i >= 14; --i) { key[i] = static_cast<wchar_t>(L'0' + (v % 10)); v /= 10; } }
    void *out = nullptr;
    void *ret = find(repo, &out, key);
    uintptr_t img = reinterpret_cast<uintptr_t>(out ? out : ret);
    if (img < 0x10000) return s;
    uintptr_t vt = 0; icon_rpm_ptr(img, vt);
    if (!(vt > er && vt - er == 0x2bb8910)) return s;          // miss = sentinel (vt != CSTextureImage)
    icon_rpm_i32(img + 0x74, s.x0); icon_rpm_i32(img + 0x78, s.y0);
    icon_rpm_i32(img + 0x7c, s.x1); icon_rpm_i32(img + 0x80, s.y1);
    uintptr_t rtex = 0; icon_rpm_ptr(img + 0x10, rtex);        // Render::Texture (bound)
    if (rtex < 0x10000) return s;
    uintptr_t res = 0; icon_rpm_ptr(rtex + 0x70, res);         // ID3D12Resource
    if (res < 0x10000) return s;
    s.sheet = reinterpret_cast<void *>(res);
    // Read W/H from the vkd3d d3d12_resource INTERNAL struct DIRECTLY (proven: dim@+0x10=3,
    // W@+0x20, H@+0x28) — NOT via GetDesc (the foreign COM call crashed). The DXGI_FORMAT lives
    // at some internal offset (found by the probe's field dump, then read here once known).
    int w = 0, h = 0; icon_rpm_i32(res + 0x20, w); icon_rpm_i32(res + 0x28, h);
    s.sheetW = static_cast<unsigned long long>(w);
    s.sheetH = static_cast<unsigned int>(h);
    s.format = 0;  // TODO: read once the internal format offset is identified (probe dump)
    s.valid = true;
    cache[iconId] = s;
    return s;
}

bool goblin::harvested_icon(int iconId, ItemSprite &out)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    auto it = g_harvest.find(iconId);
    if (it == g_harvest.end()) return false;
    out = it->second;
    return true;
}

size_t goblin::harvested_count()
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    return g_harvest.size();
}

// DEV force-load TEST — see header + findings §5b. Stream a file RESIDENT by FD4 path through the
// CSFile singleton. The loader (er+0x1f5560) is __fastcall load(CSFile, const wchar_t* path, 0, 0);
// it allocates a request, fills it from the path, and enqueues into the FD4 file/task system → an
// async load, so a bad path fails gracefully (returns null) rather than crashing. We log the handle +
// the harvested count before/after (a force-loaded icon sheet only adds repo entries once its gfx
// binds, so the count may not move immediately — the returned non-null handle is the primary signal).
void *goblin::force_load_file(const char *utf8_path)
{
    if (!goblin::config::dumpIconTextures || !utf8_path || !utf8_path[0]) return nullptr;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er) return nullptr;
    uintptr_t csfile = 0; icon_rpm_ptr(er + 0x3d5b0f8, csfile);   // CSFile FD4Singleton
    if (csfile < 0x10000) { spdlog::warn("[FORCELOAD] CSFile singleton null (not init yet)"); return nullptr; }

    wchar_t wpath[192] = {0};
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, 191) <= 0) return nullptr;

    size_t before; { std::lock_guard<std::mutex> lk(g_harvest_mtx); before = g_harvest.size(); }
    using LoadFn = void *(__fastcall *)(void *, const wchar_t *, void *, uint32_t);  // FUN_1401f5560
    auto load = reinterpret_cast<LoadFn>(er + 0x1f5560);
    void *res = load(reinterpret_cast<void *>(csfile), wpath, nullptr, 0);
    size_t after; { std::lock_guard<std::mutex> lk(g_harvest_mtx); after = g_harvest.size(); }
    spdlog::info("[FORCELOAD] load(CSFile={:#x}, '{}') -> {:#x}  harvested {}->{}",
                 csfile, utf8_path, reinterpret_cast<uintptr_t>(res), before, after);

    // sblytbnd is resident-from-boot → the load returns its cached resource WITHOUT re-decompressing
    // (Oodle hook never fires). So read the decompressed BND4 buffer straight out of the resource: it
    // holds a pointer to the BND4 bytes; probe the handle's first words via RPM (safe — bad address →
    // false, no crash), find the "BND4" buffer, RPM-copy it, and parse the item-icon layout XML.
    if (res && std::strstr(utf8_path, "sblyt") && goblin::item_icon_layout_count() == 0)
    {
        uintptr_t rp = reinterpret_cast<uintptr_t>(res);
        HANDLE self = GetCurrentProcess();
        auto try_bnd4 = [&](uintptr_t cand, uintptr_t szAt) -> bool {
            char hdr[4] = {0}; SIZE_T got = 0;
            if (cand < 0x10000 ||
                !ReadProcessMemory(self, reinterpret_cast<void *>(cand), hdr, 4, &got) || got < 4 ||
                std::memcmp(hdr, "BND4", 4) != 0)
                return false;
            uintptr_t sz = 0; if (szAt) icon_rpm_ptr(szAt, sz);
            if (sz < 0x40 || sz > 0x2000000) sz = 0x200000;
            std::vector<uint8_t> buf(sz);
            SIZE_T rd = 0;
            ReadProcessMemory(self, reinterpret_cast<void *>(cand), buf.data(), sz, &rd);
            spdlog::info("[FORCELOAD] sblyt BND4 @{:#x} size~{} read={}", cand, sz, rd);
            if (rd >= 0x40) parse_item_icon_layout(buf.data(), rd);
            return true;
        };
        bool found = false;
        // Level 1: res+off → BND4.  Level 2: res+off → obj → obj+off2 → BND4.
        for (int off = 0; off <= 0x400 && !found; off += 8)
        {
            uintptr_t c1 = 0; icon_rpm_ptr(rp + off, c1);
            if (c1 < 0x10000) continue;
            if (try_bnd4(c1, rp + off + 8)) { found = true; break; }
            for (int o2 = 0; o2 <= 0x200 && !found; o2 += 8)
            {
                uintptr_t c2 = 0; icon_rpm_ptr(c1 + o2, c2);
                if (c2 >= 0x10000 && try_bnd4(c2, c1 + o2 + 8)) found = true;
            }
        }
        if (!found)
        {
            // The raw BND4 is freed after parse. Probe whether the PARSED layout (SubTexture names +
            // rects) is persistent in RAM: follow each data pointer off the resource and scan a window
            // for "MENU_ItemIcon" (ASCII and UTF-16). Logs hits + the 24 bytes after the name (likely
            // x/y/w/h) so we can RE the rect layout. SEH-safe via ReadProcessMemory.
            auto scan_window = [&](uintptr_t base, size_t win, const char *tag) -> int {
                if (base < 0x10000) return 0;
                std::vector<uint8_t> buf(win);
                SIZE_T rd = 0;
                ReadProcessMemory(self, reinterpret_cast<void *>(base), buf.data(), win, &rd);
                if (rd < 16) return 0;
                int hits = 0;
                const char *a = "MENU_ItemIcon_";   // ASCII
                for (size_t i = 0; i + 14 < rd && hits < 4; ++i)
                    if (std::memcmp(buf.data() + i, a, 14) == 0)
                    {
                        char around[80] = {0};
                        for (int k = 0; k < 48 && i + k < rd; ++k)
                        { uint8_t c = buf[i + k]; around[k] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
                        spdlog::info("[LAYOUT-PROBE] {} +{:#x} ASCII hit: '{}'", tag, i, around);
                        ++hits;
                    }
                // UTF-16: 'M',0,'E',0,'N',0,'U',0 ...
                static const uint8_t w[] = {'M',0,'E',0,'N',0,'U',0,'_',0,'I',0};
                for (size_t i = 0; i + sizeof(w) < rd && hits < 8; ++i)
                    if (std::memcmp(buf.data() + i, w, sizeof(w)) == 0)
                    { spdlog::info("[LAYOUT-PROBE] {} +{:#x} UTF16 'MENU_I...' hit", tag, i); ++hits; }
                return hits;
            };
            int total = 0;
            uintptr_t p48 = 0; icon_rpm_ptr(rp + 0x48, p48);
            total += scan_window(p48, 0x40000, "res+0x48");
            // also follow one level: each pointer inside the +48 object
            for (int o = 0; o <= 0x80 && total == 0; o += 8)
            { uintptr_t pp = 0; icon_rpm_ptr(p48 + o, pp); total += scan_window(pp, 0x40000, "res+0x48->"); }
            if (total == 0)
                spdlog::info("[LAYOUT-PROBE] no MENU_ItemIcon names found near the resource — parsed layout "
                             "is hashed or elsewhere (handle {:#x}, +48={:#x})", rp, p48);
        }
    }
    return res;
}

// ── DEV bind-flip TEST (RE findings §5e) ─────────────────────────────────────────────────────────
// Question: does setting a loaded resource-GROUP entry's +0x7c "needs-apply" flag trigger the binding
// (the apply vmethod resource+0xc8) that populates the repo with per-icon RECTS — i.e. can we force a
// non-resident item-icon group resident + bound on demand? We hook the residency ticker FUN_140d724c0
// to (a) capture the live menu-resource manager (*param_2 — it is reached via the task system, not a
// flat singleton) and (b) run a one-shot action INLINE on the engine thread BEFORE calling the
// original, so the original's per-tick group-apply (FUN_140d78540) consumes our flag THIS tick.
// All memory access is RPM/WPM (clang-cl elides __try around raw derefs); the by-groupId loaders are
// called directly (same as force_load_file). Gated by config::dumpIconTextures; driven from the P2b panel.
namespace
{
using res_tick_fn = void(__fastcall *)(uintptr_t, uintptr_t *);
res_tick_fn g_res_tick_orig = nullptr;
std::atomic<uintptr_t> g_res_mgr{0};
std::atomic<int> g_bind_action{0};   // 0 idle | 1 dump | 2 load files | 3 flip-bind | 4 load+flip
std::atomic<int> g_bind_gid{1};      // group id for the loaders (1 = 01_Common = the item-icon group)

// ── CreateImage force-bind (RE §5g) ──────────────────────────────────────────────────────────────
// CSScaleformImageCreator::CreateImage (FUN_140d6bbc0) is hooked above (create_image_detour, the
// existing probe hook) which captures the live context g_ci_p1/g_ci_p2. Here we replay it: build a
// synthetic symbol desc for "MENU_ItemIcon_<id>" and call the original via g_create_image_orig.
constexpr int CI_REPLAY_LAST = INT_MIN + 1;   // sentinel: replay the last captured live symbol
std::atomic<int> g_ci_req_icon{INT_MIN};   // queued iconId (INT_MIN = idle, CI_REPLAY_LAST = replay)
alignas(16) char g_ci_name[96];            // 4-aligned scratch for the synthesized symbol name
uintptr_t g_ci_desc[4] = {0};              // synthetic param_3: [0] = (g_ci_name - 0xc), rest 0 (pad)

// ── CENTRAL GPU-ICON REGISTRY (the one place that owns "which native icons must be resident") ──────
// The render side REGISTERS what it wants (item iconIds for category/loot icons, MENU_MAP_* /
// SB_ERR_* symbol names for world-feature pins like bosses & graces). The engine-thread tick
// (gpu_icon_tick, in res_tick_detour) ALWAYS re-forces the whole wanted set in a round-robin —
// no try-cap, no "harvest once then hope" — so an icon evicted by repo churn self-heals on the next
// pass. Forcing goes through the HOOKED CreateImage (er+0xd6bbc0) so create_image_detour actually
// harvests the result (the old run_create_icon used the ORIG → never harvested → the 129->129 bug).
std::mutex g_want_mx;
std::set<int> g_want_items;            // wanted MENU_ItemIcon_<id>
std::set<std::string> g_want_symbols;  // wanted img://MENU_MAP_* / SB_ERR_* symbol names

inline bool res_w32(uintptr_t a, uint32_t v)
{
    SIZE_T n = 0;
    return a && WriteProcessMemory(GetCurrentProcess(), (void *)a, &v, 4, &n) && n == 4;
}

// Walk the manager's loaded-group array (base = align8(mgr+0x9d8), stride 0x10, count mgr+0xbe0; each
// slot holds a POINTER to a group entry). Log each entry's resource obj (+0x18), its apply vmethod
// (resource vtable +0xc8 as an RVA — THIS identifies what the bind does, cf. §5c parse fns), and the
// flags (+0x7c needs-apply, +0x80 applied). If `flip`, set +0x7c=1 to force a re-apply this tick.
void res_walk_groups(uintptr_t mgr, uintptr_t er, bool flip)
{
    uintptr_t count = 0;
    if (!icon_rpm_ptr(mgr + 0xbe0, count) || count == 0 || count > 4096)
    {
        spdlog::warn("[BINDTEST] implausible group count {} @ mgr={:#x} — aborting (wrong manager?)", count, mgr);
        return;
    }
    uintptr_t base = (mgr + 0x9d8 + 7) & ~uintptr_t(7);   // engine's (-(addr)&7) 8-byte pad
    // Game-thread per-node RPM walk (res_tick_detour, F1 BINDTEST) → keep it bench-visible
    // (docs/rpm_walk_audit.md).
    GOBLIN_BENCH_QUIET("bindtest.group_walk");
    int flipped = 0;
    for (uintptr_t i = 0; i < count; ++i)
    {
        uintptr_t entry = 0;
        if (!icon_rpm_ptr(base + i * 0x10, entry) || entry < 0x10000) continue;
        int f7c = 0, f80 = 0;
        icon_rpm_i32(entry + 0x7c, f7c);
        icon_rpm_i32(entry + 0x80, f80);
        uintptr_t res = 0, vt = 0, apply = 0;
        icon_rpm_ptr(entry + 0x18, res);
        if (res > 0x10000) { icon_rpm_ptr(res, vt); if (vt) icon_rpm_ptr(vt + 0xc8, apply); }
        spdlog::info("[BINDTEST] grp[{}] entry={:#x} res={:#x} apply(vt+0xc8)={:#x} RVA={:#x} +0x7c={} +0x80={}",
                     i, entry, res, apply, (apply > er) ? apply - er : 0, f7c, f80);
        if (flip && res_w32(entry + 0x7c, 1)) ++flipped;
    }
    if (flip)
    {
        res_w32(mgr + 0x1e19, 1);   // dirty flag (also drains the +0x1df0 load queue next tick)
        spdlog::info("[BINDTEST] flipped +0x7c on {} groups + set dirty +0x1e19 — orig FUN_140d78540 applies now", flipped);
    }
}

void run_bind_action(uintptr_t mgr, uintptr_t er, int act, int gid)
{
    spdlog::info("[BINDTEST] action={} gid={} mgr={:#x} harvested={} (before)", act, gid, mgr,
                 goblin::harvested_count());
    if ((act == 2 || act == 4) && gid >= 0 && gid <= 8)
    {
        // By-groupId loaders (FUN_140d77550 = TPF, FUN_140d771d0 = sblytbnd). UNLIKE force_load_file's
        // raw CSFile call, these cache the handle at mgr+0xd10/+0xd58+gid*8 — exactly where the bind's
        // apply vmethod looks for the loaded resource. Guarded internally (no-op if already loaded).
        reinterpret_cast<void(__fastcall *)(uintptr_t, uint8_t)>(er + 0xd77550)(mgr, (uint8_t)gid);
        reinterpret_cast<void(__fastcall *)(uintptr_t, uint8_t)>(er + 0xd771d0)(mgr, (uint8_t)gid);
        spdlog::info("[BINDTEST] called FUN_140d77550 + FUN_140d771d0 (mgr, gid={})", gid);
    }
    res_walk_groups(mgr, er, /*flip=*/(act == 3 || act == 4));
}

// Force-call CreateImage for "MENU_ItemIcon_<iconId>" (engine thread, via the ticker request). Builds a
// synthetic symbol desc: param_3 → a qword holding (nameAddr-0xc) so (*p3 & ~3)+0xc == nameAddr (the
// 4-aligned ASCII name). Calls the ORIGINAL via g_create_image_orig (the hook trampoline) with the
// captured creator context. Logs the returned image + whether the repo grew. Caveat (§5b): the
// "<name>_ptl" view resolves against a RESIDENT sheet — pair with "Load files (gid)" if not loaded.
void run_create_icon(uintptr_t er, int iconId)
{
    (void)er;
    if (!g_create_image_orig) { spdlog::warn("[CREATEIMG] no orig (probe hook missing)"); return; }
    if (iconId == CI_REPLAY_LAST)
    {
        if (!g_ci_last[0]) { spdlog::warn("[CREATEIMG] no live symbol captured yet"); return; }
        strncpy(g_ci_name, g_ci_last, sizeof(g_ci_name) - 1);   // replay a known-good symbol (control)
    }
    else
    {
        // GFx import scheme is "img://<name>" (live names showed img://KG_R1 etc.), base = MENU_ItemIcon_<id>.
        snprintf(g_ci_name, sizeof(g_ci_name), "img://MENU_ItemIcon_%d", iconId);
    }
    g_ci_desc[0] = reinterpret_cast<uintptr_t>(g_ci_name) - 0xc;   // (desc[0] & ~3)+0xc == g_ci_name
    size_t before = goblin::harvested_count();
    void *img = g_create_image_orig(g_ci_p1.load(), g_ci_p2.load(), g_ci_desc, nullptr, nullptr,
                                    nullptr, nullptr, nullptr);
    size_t after = goblin::harvested_count();
    spdlog::info("[CREATEIMG] force '{}' (p1={} p2={}) -> img={:#x}  harvested {}->{}", g_ci_name,
                 g_ci_p1.load(), g_ci_p2.load(), reinterpret_cast<uintptr_t>(img), before, after);
}

// Force the REAL grace icon (the mod's gfx sprite, NOT the native SB_ERR time-tinted pin). repo+0x80 =
// MENU_ItemIcon, repo+0xb0 = MENU_MapTile — neither holds the pin sprites, so we force-create the gfx
// img:// imports via the HOOKED CreateImage (the proven path: KG_/SB_ERR resolved). Each result lands
// in g_icon_imgs (via create_image_detour) → dump_icon_textures_live captures it as a grace candidate.
// MENU_MAP_GOBLIN_Grace = canonical; siblings populate the F1 picker. Engine thread; throttled by caller.
int g_grace_force_tries = 0;
std::atomic<bool> g_force_grace_req{false}; // manual F1 "Force graces now" (bypasses the auto cap)
void run_force_grace(uintptr_t er)
{
    if (!g_create_image_orig || g_ci_p1.load() == nullptr) return;
    static const char *kCands[] = {
        "img://MENU_MAP_01_Bonfire",            // the REAL vanilla grace icon (bonfire = grace)
        "img://MENU_MAP_GOBLIN_Grace", "img://MENU_MAP_GOBLIN_SortaGraceIDK",
        "img://MENU_MAP_ERR_GraceUnderground", "img://SB_ERR_Grace_Morning_Color",
    };
    auto CreateImageHooked = reinterpret_cast<create_image_fn>(er + 0xd6bbc0);
    for (const char *c : kCands)
    {
        snprintf(g_ci_name, sizeof(g_ci_name), "%s", c);
        g_ci_desc[0] = reinterpret_cast<uintptr_t>(g_ci_name) - 0xc;
        void *img = CreateImageHooked(g_ci_p1.load(), g_ci_p2.load(), g_ci_desc, nullptr, nullptr,
                                      nullptr, nullptr, nullptr);
        if (goblin::config::dumpIconTextures)   // gated: the central pump calls this continuously
            spdlog::info("[GRACE-FORCE] CreateImage('{}') -> {:#x}", c, reinterpret_cast<uintptr_t>(img));
    }
}

// ── Central force primitives (HOOKED CreateImage → result is harvested by create_image_detour) ─────
// Force one GFx image symbol resident via the hooked CreateImage. Engine thread; needs a captured
// context (g_ci_p1). This is the SINGLE force path for everything (items + map symbols).
static void force_symbol_hooked(uintptr_t er, const char *imgname)
{
    if (!g_create_image_orig || g_ci_p1.load(std::memory_order_relaxed) == nullptr || !imgname) return;
    auto CreateImageHooked = reinterpret_cast<create_image_fn>(er + 0xd6bbc0);
    snprintf(g_ci_name, sizeof(g_ci_name), "%s", imgname);
    g_ci_desc[0] = reinterpret_cast<uintptr_t>(g_ci_name) - 0xc;
    CreateImageHooked(g_ci_p1.load(), g_ci_p2.load(), g_ci_desc, nullptr, nullptr, nullptr, nullptr, nullptr);
}
static void force_item_hooked(uintptr_t er, int iconId)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "img://MENU_ItemIcon_%d", iconId);
    force_symbol_hooked(er, buf);
}

// THE central engine-thread pump (called every res tick): ALWAYS re-force the whole wanted set in a
// round-robin so anything evicted by repo churn comes back, + re-harvest the twin-map symbols
// (boss/grace pins) so their rects stay registered. No try-cap. Throttled so it costs little.
void gpu_icon_tick(uintptr_t er)
{
    if (g_ci_p1.load(std::memory_order_relaxed) == nullptr) return;  // no CreateImage context yet
    static int s_tick = 0;
    if ((s_tick++ % 6) != 0) return;  // act ~every 6 ticks (cheap; still continuous)

    // Item-icon LAYOUT (iconId→sheet+rect) disk-load was HOISTED to background_harvest_tick() (worldmap
    // probe thread) — it is pure file IO with no CreateImage/manager dependency, so it must not wait for
    // this res_tick-gated path (map-only sessions never reach here). See background_harvest_tick().

    // Grace candidates (their own hardcoded set) — always, no cap.
    if (goblin::config::graceOverlay && goblin::config::graceGpuSprite)
        run_force_grace(er);

    // Snapshot the wanted set (brief lock; force calls happen unlocked).
    std::vector<int> items;
    std::vector<std::string> syms;
    {
        std::lock_guard<std::mutex> lk(g_want_mx);
        items.assign(g_want_items.begin(), g_want_items.end());
        syms.assign(g_want_symbols.begin(), g_want_symbols.end());
    }
    // Round-robin: force a handful per pass so we cycle the whole set over a few passes (continuous).
    static size_t ri = 0, rs = 0;
    for (int k = 0; k < 6 && !items.empty(); ++k) force_item_hooked(er, items[ri++ % items.size()]);
    for (int k = 0; k < 3 && !syms.empty(); ++k)  force_symbol_hooked(er, syms[rs++ % syms.size()].c_str());

    // Re-register the resident map symbols into g_map_icon_named/_rects. The MENU_MAP_* pins (boss!)
    // live in repo+0x80 → harvest_repo_icons; the MapTile twin is repo+0xb0 → harvest_twin_map_icons.
    // RPM-heavy (full repo walks) → run them less often than the cheap CreateImage forces above.
    static int s_harvest = 0;
    if ((s_harvest++ % 5) == 0)
    {
        harvest_repo_icons();                                   // repo+0x80: MENU_MAP_* + MENU_ItemIcon_*
        if (g_icon_repo) harvest_twin_map_icons(g_icon_repo, er); // repo+0xb0: MapTile twin
    }

    // [BOSSDIAG] one-shot-ish census of the boss map symbol: is it registered, under which key, and
    // does the bare-name lookup resolve? Pinpoints registration-vs-lookup-vs-copy. Throttled + gated.
    if (goblin::config::dumpIconTextures)
    {
        static int s_bd = 0;
        if ((s_bd++ % 20) == 0)
        {
            int n = 0; std::string keys;
            {
                std::lock_guard<std::mutex> lk(g_map_icon_mtx);
                for (const auto &kv : g_map_icon_named)
                    if (kv.first.find("Boss") != std::string::npos)
                    { if (n++ < 6) { keys += kv.first; keys += ' '; } }
            }
            int bx, by, bw, bh; void *bsheet = nullptr;
            bool hit = goblin::map_icon_rect_by_name("MENU_MAP_ERR_Boss", bx, by, bw, bh, bsheet);
            spdlog::info("[BOSSDIAG] named_keys_with_Boss={} [{}] bare_lookup_hit={} sheet={:#x}",
                         n, keys, hit, reinterpret_cast<uintptr_t>(bsheet));
        }
    }
}

void __fastcall res_tick_detour(uintptr_t p1, uintptr_t *p2)
{
    if (p2)
    {
        uintptr_t mgr = *p2;
        if (mgr > 0x10000)
        {
            g_res_mgr.store(mgr, std::memory_order_relaxed);
            uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
            int act = g_bind_action.exchange(0, std::memory_order_acq_rel);
            if (act) run_bind_action(mgr, er, act, g_bind_gid.load(std::memory_order_relaxed));
            int ci = g_ci_req_icon.exchange(INT_MIN, std::memory_order_acq_rel);
            if (ci != INT_MIN) run_create_icon(er, ci);   // F1 dev probe (orig call, control)
            // Manual F1 "Force graces now" — one-shot poke (bypasses the auto pump's throttle).
            if (g_force_grace_req.exchange(false, std::memory_order_acq_rel) &&
                g_ci_p1.load(std::memory_order_relaxed))
            {
                run_force_grace(er);
                if (g_icon_repo)
                    harvest_twin_map_icons(g_icon_repo, er);
            }
            // THE central GPU-icon pump: always re-force the whole wanted set (items + map symbols)
            // + re-harvest, so nothing stays evicted. Replaces the old capped grace force + the
            // per-category drain (both folded in here). Self-throttled.
            gpu_icon_tick(er);
        }
    }
    if (g_res_tick_orig) g_res_tick_orig(p1, p2);
}
}  // namespace

// Public driver (P2b panel). Queues a one-shot action consumed on the next residency tick (engine
// thread). action: 1=dump groups, 2=load files(gid), 3=flip-bind all loaded groups, 4=load+flip(gid).
// Returns false if the ticker hasn't captured the manager yet (open the inventory/map once).
bool goblin::bind_test(int action, int groupId)
{
    if (!goblin::config::dumpIconTextures) return false;
    if (g_res_mgr.load(std::memory_order_relaxed) == 0)
    {
        spdlog::warn("[BINDTEST] manager not captured yet — open the inventory or map once (ticker must run)");
        return false;
    }
    g_bind_gid.store(groupId, std::memory_order_relaxed);
    g_bind_action.store(action, std::memory_order_release);
    return true;
}

// Queue a force-CreateImage for "MENU_ItemIcon_<iconId>" — consumed on the next residency tick (engine
// thread). The CreateImage hook must have captured a live context first (open the inventory once).
bool goblin::force_create_icon(int iconId)
{
    if (!goblin::config::dumpIconTextures) return false;
    if (g_res_mgr.load(std::memory_order_relaxed) == 0)
    {
        spdlog::warn("[CREATEIMG] manager not captured yet — open the inventory/map once");
        return false;
    }
    g_ci_req_icon.store(iconId, std::memory_order_release);
    return true;
}

// Queue a per-category representative item-icon to be force-made-resident on the engine thread (the
// res_tick drain above). Called by the map build for each category's resolved iconId. Bounded +
// deduped; safe to call from any thread. Not gated by dumpIconTextures — this is a shipping feature
// path (the drain itself no-ops until a CreateImage context is captured).
// Register an item iconId (MENU_ItemIcon_<id>) the renderer wants resident. The central pump
// (gpu_icon_tick) keeps it force-loaded. Idempotent, thread-safe, bounded.
void goblin::gpu_want_item(int iconId)
{
    if (iconId <= 0) return;
    std::lock_guard<std::mutex> lk(g_want_mx);
    if (g_want_items.size() < 1024) g_want_items.insert(iconId);
}
// Register a named map symbol (e.g. "img://MENU_MAP_ERR_Boss") the renderer wants resident.
void goblin::gpu_want_symbol(const char *imgName)
{
    if (!imgName || !imgName[0]) return;
    std::lock_guard<std::mutex> lk(g_want_mx);
    if (g_want_symbols.size() < 256) g_want_symbols.insert(imgName);
}

// Manually re-run the grace force-CreateImage (F1 dev button) — consumed on the next residency
// tick (engine thread), bypassing the auto cap/lock. Harvests the MENU_MAP_*/SB_ERR_Grace_*
// gfx-movie sprites into the candidate list. Returns false if the ticker/context isn't ready yet.
bool goblin::force_graces()
{
    if (!goblin::config::dumpIconTextures) return false;
    if (g_res_mgr.load(std::memory_order_relaxed) == 0)
    {
        spdlog::warn("[GRACE-FORCE] manager not captured yet — open the inventory/map once");
        return false;
    }
    g_force_grace_req.store(true, std::memory_order_release);
    return true;
}

// Control test: replay the LAST captured live symbol (a known-good img:// import) to prove the replay
// mechanism end-to-end. If this returns a valid img but force_create_icon(id) returns 0, the item-icon
// base simply isn't a CreateImage import (it's the widget's direct repo find / sblytbnd bind path).
bool goblin::force_create_last()
{
    if (!goblin::config::dumpIconTextures) return false;
    g_ci_req_icon.store(CI_REPLAY_LAST, std::memory_order_release);
    return true;
}

bool goblin::harvested_grace(ItemSprite &out)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    if (!g_grace_sprite.valid) return false;
    out = g_grace_sprite;
    return true;
}

std::vector<goblin::GraceCandidate> goblin::grace_candidates()
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    return g_grace_cands;
}

// The ERR dungeon-style grace sprite (MENU_MAP_ERR_GraceUnderground). False until captured / if ERR
// isn't installed (sprite absent) → renderer falls back to the vanilla grace for dungeon graces too.
bool goblin::harvested_grace_dungeon(ItemSprite &out)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    if (!g_grace_dungeon_sprite.valid) return false;
    out = g_grace_dungeon_sprite;
    return true;
}

// DEV (F1 grace-debug): set the active grace sprite from a captured candidate by index, so the
// overlay can test which name maps to the correct grace. Also locks it so the auto-capture won't
// override. Returns false on a bad index. The overlay must then force_rebuild_grace() to re-copy.
bool goblin::set_grace_from_candidate(size_t idx)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    if (idx >= g_grace_cands.size() || !g_grace_cands[idx].spr.valid) return false;
    g_grace_sprite = g_grace_cands[idx].spr;
    g_grace_locked = true;
    spdlog::info("[GRACE-DBG] active grace set to candidate[{}] '{}'", idx, g_grace_cands[idx].name);
    return true;
}

std::vector<int> goblin::harvested_ids(size_t max)
{
    std::lock_guard<std::mutex> lk(g_harvest_mtx);
    std::vector<int> out;
    out.reserve(g_harvest.size() < max ? g_harvest.size() : max);
    for (const auto &kv : g_harvest)
    {
        if (out.size() >= max) break;
        out.push_back(kv.first);
    }
    return out;
}

void goblin::probe_icon_find_runtime()
{
    if (!goblin::config::dumpIconTextures)
        return;
    // ⛔ DISABLED: find-by-name (resolve_item_sprite) CRASHES DETERMINISTICALLY inside
    // FUN_140d63c30 (exe+0xD63E02, fault exe+0x591401F) when the queried icon's sheet is NOT fully
    // resident — for not-loaded ids it returns a clean miss, but for a partially-referenced id it
    // derefs an unready field and faults. clang-cl can't SEH-guard the foreign call. The earlier
    // [FIND2-TEX] successes were a lucky inventory state. → the SAFE path is enumerate-LOADED images
    // (sprite findings §1: FUN_140d69640 walks the loaded movie's image list — only resident images,
    // no risky by-name query) + integrate on the RENDER thread, not this hotkey thread. resolve_item_sprite
    // stays for resident-only use. See [[overlay-icon-atlas]].
    spdlog::warn("[FIND2] find-by-name probe DISABLED — crashes on non-resident sheets; "
                 "switch to enumerate-loaded (FUN_140d69640). See memory.");
}

// ── OodleLZ_Decompress hook (FD4 RAM-cache RE, windows_fd4_ram_dds_cache_re_prompt.md) ───────────
// The menu texture file 01_common.tpf.dcx is DCX/KRAK-compressed; the engine decompresses it via
// oo2core's OodleLZ_Decompress into a CPU buffer (the TPF, whose entries are the icon-sheet DDS).
// Hooking it logs that buffer's address + size at the moment of decompression — the candidate for a
// persistent CPU DDS cache (read it again menu-closed to test persistence). dst is in OUR address
// space (post-decompress) so we read it directly. 14-arg Oodle signature; we pass everything through.
using oodle_decompress_fn = long long(__fastcall *)(const void *, long long, void *, long long, int,
                                                    int, int, void *, long long, void *, void *,
                                                    void *, long long, int);
oodle_decompress_fn g_oodle_orig = nullptr;
// ER's decompressed TPF buffers are TRANSIENT (freed after it parses them), so we COPY each complete
// DDS out IN the hook (while valid) and ACCUMULATE distinct ones — a browser to find the icon sheet.
std::mutex g_tpf_mtx;
std::vector<std::vector<uint8_t>> g_dds_list; // all distinct complete DDS captured from decompresses

// ── Item-icon LAYOUT, captured no-bake from the sblytbnd XML ─────────────────────────────────────
// The menu sblytbnd (decompressed by Oodle) holds TextureAtlas XML: <SubTexture name="MENU_ItemIcon_
// <id>.png" x= y= width= height=/> under an enclosing imagePath="<sheet>". We string-scan that out
// of the decompressed buffer → iconId → (sheet, rect), so we can crop a NON-resident item icon from
// its sheet DDS without the engine streaming it. Mirrors what extract_subtextures.py does offline.
struct ItemIconRect { int x, y, w, h; std::string sheet; };
std::mutex g_item_layout_mtx;
std::map<int, ItemIconRect> g_item_icon_layout;   // iconId -> rect+sheet (from sblytbnd)

// Map-point glyphs (SB_MapCursor[_ERR]) parsed from the SAME disk sblytbnd. Standard points are
// MENU_MAP_<NN> (NN = WorldMapPointParam.iconId) → keyed by iconId; ERR custom points are named
// (MENU_MAP_ERR_Boss …) → keyed by full name. Lets native_map_point_icon draw a POI pin from disk
// (Oodle no-bake) without waiting for the engine to make the symbol resident. See
// docs/re/windows_map_point_icon_layout_re_findings.md.
struct MapPointRect { int x, y, w, h; std::string sheet; };
std::map<int, MapPointRect> g_map_point_layout;          // numeric MENU_MAP_<id> → rect+sheet
std::map<std::string, MapPointRect> g_map_point_named;    // "MENU_MAP_ERR_*" → rect+sheet

// Find `needle` in [hay, hay+n). Returns nullptr if absent (no memmem on Windows CRT).
static const char *find_sub(const char *hay, size_t n, const char *needle, size_t nl)
{
    if (nl == 0 || n < nl) return nullptr;
    for (const char *p = hay; p <= hay + n - nl; ++p)
        if (p[0] == needle[0] && std::memcmp(p, needle, nl) == 0) return p;
    return nullptr;
}
// Parse `attr="NNN"` (integer) within [p, endp). -1 if absent.
static int xml_int_attr(const char *p, const char *endp, const char *attr)
{
    const size_t al = std::strlen(attr);
    for (const char *q = p; q + al + 2 < endp; ++q)
        if (q[0] == attr[0] && std::memcmp(q, attr, al) == 0 && q[al] == '=' && q[al + 1] == '"')
            return std::atoi(q + al + 2);
    return -1;
}

// Scan a decompressed sblytbnd BND4 blob for MENU_ItemIcon_<id> SubTextures → g_item_icon_layout.
void parse_item_icon_layout(const uint8_t *buf, size_t n)
{
    const char *b = reinterpret_cast<const char *>(buf);
    const char *end = b + n;
    static const char NEEDLE[] = "MENU_ItemIcon_";
    const size_t NL = sizeof(NEEDLE) - 1;
    static const char IP[] = "imagePath=\"";
    const size_t IPL = sizeof(IP) - 1;
    int added = 0, skipped_nosheet = 0;
    std::lock_guard<std::mutex> lk(g_item_layout_mtx);
    // Forward pass tracking the CURRENT atlas imagePath. The old per-SubTexture backward scan was capped
    // at 16KB, but a 4096x2048 / 160px sheet holds ~300 SubTextures (~22KB of XML), so entries late in a
    // big atlas sat past the window → empty sheet → stored anyway → drawn from the baked atlas forever.
    // Tracking imagePath as we advance is O(n) and correct regardless of atlas size.
    std::string cur_sheet;
    const char *ip_scan = b;   // imagePath tags consumed up to here
    for (const char *p = b; p + NL < end; )
    {
        const char *m = find_sub(p, end - p, NEEDLE, NL);
        if (!m) break;
        // Consume every imagePath=" tag in [ip_scan, m) so cur_sheet = the atlas this SubTexture is under.
        while (ip_scan < m)
        {
            const char *ip = find_sub(ip_scan, m - ip_scan, IP, IPL);
            if (!ip) break;
            const char *s = ip + IPL;
            const char *e = static_cast<const char *>(std::memchr(s, '"', end - s));
            cur_sheet = (e && e > s) ? std::string(s, e) : std::string();
            ip_scan = e ? e + 1 : ip + IPL;
        }
        const char *tagEnd = static_cast<const char *>(std::memchr(m, '>', end - m));
        int id = std::atoi(m + NL);
        if (id > 0 && tagEnd)
        {
            int x = xml_int_attr(m, tagEnd, "x"), y = xml_int_attr(m, tagEnd, "y");
            int w = xml_int_attr(m, tagEnd, "width"), h = xml_int_attr(m, tagEnd, "height");
            if (w > 0 && h > 0)
            {
                if (cur_sheet.empty())
                {
                    // No resolvable sheet → can't draw it natively; leave it out so item_icon_layout_rect
                    // returns false and native_item_icon falls cleanly to the baked atlas (no '' request).
                    ++skipped_nosheet;
                }
                else
                {
                    ItemIconRect &r = g_item_icon_layout[id];
                    r.x = x; r.y = y; r.w = w; r.h = h; r.sheet = cur_sheet;
                    ++added;
                }
            }
        }
        p = tagEnd ? tagEnd + 1 : m + NL;
    }
    if (added || skipped_nosheet)
        spdlog::info("[ITEMLAYOUT] parsed {} MENU_ItemIcon rects (total distinct {}; {} skipped, no sheet)",
                     added, g_item_icon_layout.size(), skipped_nosheet);
}

// Scan the SAME decompressed sblytbnd blob for MENU_MAP_* SubTextures (SB_MapCursor[_ERR] glyphs).
// Standard MENU_MAP_<NN> (NN = WorldMapPointParam.iconId) → g_map_point_layout[iconId]; ERR custom
// MENU_MAP_ERR_* → g_map_point_named. Same forward imagePath-tracking scan as the item parser. The
// per-entry [MAPPTLAYOUT] log is a one-shot discovery trace (reveals the ERR iconId↔name↔sheet map);
// trim it to a summary once the POI wiring is settled.
void parse_map_point_layout(const uint8_t *buf, size_t n)
{
    const char *b = reinterpret_cast<const char *>(buf);
    const char *end = b + n;
    static const char NEEDLE[] = "MENU_MAP_";
    const size_t NL = sizeof(NEEDLE) - 1;
    static const char IP[] = "imagePath=\"";
    const size_t IPL = sizeof(IP) - 1;
    int num = 0, named = 0;
    std::lock_guard<std::mutex> lk(g_item_layout_mtx);
    std::string cur_sheet;
    const char *ip_scan = b;
    for (const char *p = b; p + NL < end; )
    {
        const char *m = find_sub(p, end - p, NEEDLE, NL);
        if (!m) break;
        while (ip_scan < m)
        {
            const char *ip = find_sub(ip_scan, m - ip_scan, IP, IPL);
            if (!ip) break;
            const char *s = ip + IPL;
            const char *e = static_cast<const char *>(std::memchr(s, '"', end - s));
            cur_sheet = (e && e > s) ? std::string(s, e) : std::string();
            ip_scan = e ? e + 1 : ip + IPL;
        }
        const char *tagEnd = static_cast<const char *>(std::memchr(m, '>', end - m));
        // token = chars after "MENU_MAP_" up to the '.' in name="MENU_MAP_<token>.png"
        const char *t = m + NL, *te = t;
        while (te < end && (std::isalnum(static_cast<unsigned char>(*te)) || *te == '_')) ++te;
        std::string token(t, te);
        if (tagEnd && !token.empty() && !cur_sheet.empty())
        {
            int x = xml_int_attr(m, tagEnd, "x"), y = xml_int_attr(m, tagEnd, "y");
            int w = xml_int_attr(m, tagEnd, "width"), h = xml_int_attr(m, tagEnd, "height");
            if (w > 0 && h > 0)
            {
                bool numeric = token.find_first_not_of("0123456789") == std::string::npos;
                if (numeric)
                {
                    MapPointRect &r = g_map_point_layout[std::atoi(token.c_str())];
                    r.x = x; r.y = y; r.w = w; r.h = h; r.sheet = cur_sheet;
                    ++num;
                }
                else
                {
                    MapPointRect &r = g_map_point_named["MENU_MAP_" + token];
                    r.x = x; r.y = y; r.w = w; r.h = h; r.sheet = cur_sheet;
                    ++named;
                }
                spdlog::debug("[MAPPTLAYOUT] MENU_MAP_{} sheet={} rect=({},{},{},{})",
                              token, cur_sheet, x, y, w, h);
            }
        }
        p = tagEnd ? tagEnd + 1 : m + NL;
    }
    if (num || named)
        spdlog::info("[MAPPTLAYOUT] parsed {} numeric + {} named MENU_MAP rects (distinct {}/{})",
                     num, named, g_map_point_layout.size(), g_map_point_named.size());
}

size_t goblin::item_icon_layout_count()
{
    std::lock_guard<std::mutex> lk(g_item_layout_mtx);
    return g_item_icon_layout.size();
}

bool goblin::item_icon_layout_rect(int iconId, int &x, int &y, int &w, int &h, std::string &sheet)
{
    std::lock_guard<std::mutex> lk(g_item_layout_mtx);
    auto it = g_item_icon_layout.find(iconId);
    if (it == g_item_icon_layout.end()) return false;
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

// Map-point glyph rect lookups (SB_MapCursor[_02] from the disk sblytbnd). By full name
// ("MENU_MAP_Player_02", "MENU_MAP_ERR_Boss") or by numeric WorldMapPointParam iconId.
bool goblin::map_point_rect_by_name(const std::string &name, int &x, int &y, int &w, int &h, std::string &sheet)
{
    std::lock_guard<std::mutex> lk(g_item_layout_mtx);
    auto it = g_map_point_named.find(name);
    if (it == g_map_point_named.end() || it->second.sheet.empty()) return false;
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

bool goblin::map_point_rect(int iconId, int &x, int &y, int &w, int &h, std::string &sheet)
{
    std::lock_guard<std::mutex> lk(g_item_layout_mtx);
    auto it = g_map_point_layout.find(iconId);
    if (it == g_map_point_layout.end() || it->second.sheet.empty()) return false;
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

bool goblin::load_item_icon_layout_from_disk()
{
    if (item_icon_layout_count() > 0) return true;  // already captured (this path or the RAM hook)
    // Read the active mod's (overlay) or the UXM-unpacked game's real menu sblytbnd → decompressed
    // BND4 whose .layout entries are plaintext TextureAtlas XML. parse_item_icon_layout string-scans
    // the MENU_ItemIcon_<id> SubTextures straight out of it (no BND4 entry walk needed).
    std::vector<uint8_t> bnd =
        goblin::worldmap::read_game_file_decompressed("menu/hi/01_common.sblytbnd.dcx");
    if (bnd.size() < 0x40)
    {
        spdlog::warn("[ITEMLAYOUT] disk sblytbnd unavailable ({} bytes) — layout not loaded",
                     bnd.size());
        return false;
    }
    if (!(bnd[0] == 'B' && bnd[1] == 'N' && bnd[2] == 'D' && bnd[3] == '4'))
        spdlog::warn("[ITEMLAYOUT] disk sblytbnd decompressed but not BND4 "
                     "({:02x}{:02x}{:02x}{:02x}) — parsing the XML anyway",
                     bnd[0], bnd[1], bnd[2], bnd[3]);
    parse_item_icon_layout(bnd.data(), bnd.size());
    parse_map_point_layout(bnd.data(), bnd.size());   // same blob also holds SB_MapCursor[_ERR] glyphs
    size_t n = item_icon_layout_count();
    spdlog::info("[ITEMLAYOUT] from disk sblytbnd: {} MENU_ItemIcon rects", n);
    return n > 0;
}

long long __fastcall oodle_decompress_detour(const void *src, long long srcLen, void *dst,
                                             long long dstLen, int a5, int a6, int a7, void *a8,
                                             long long a9, void *a10, void *a11, void *a12,
                                             long long a13, int a14)
{
    long long r = g_oodle_orig(src, srcLen, dst, dstLen, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    // No-bake item-icon LAYOUT capture: the sblytbnd decompresses to a BND4 holding the TextureAtlas
    // XML. Scan it for MENU_ItemIcon rects (cheap: only BND4 blobs, only when item icons are on).
    if ((goblin::config::nativeItemIcons || goblin::config::dumpIconTextures) && dst && r >= 0x40)
    {
        const uint8_t *bb = reinterpret_cast<const uint8_t *>(dst);
        if (bb[0] == 'B' && bb[1] == 'N' && bb[2] == 'D' && bb[3] == '4' &&
            find_sub(reinterpret_cast<const char *>(bb), (size_t)r, "MENU_ItemIcon_", 14))
            parse_item_icon_layout(bb, (size_t)r);
    }
    if (goblin::config::dumpIconTextures && dst && r >= 0x10000)
    {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(dst);
        // Decompressed menu texture container = a TPF ("TPF\0"). Log its RAM address + size so we can
        // re-read it menu-closed (persistence test) and parse DDS+rects out of it.
        if (b[0] == 'T' && b[1] == 'P' && b[2] == 'F' && b[3] == 0 && r >= 0x20)
        {
            uint32_t fc = 0, doff = 0, dsz = 0;
            std::memcpy(&fc, b + 0x08, 4);
            std::memcpy(&doff, b + 0x10, 4);
            std::memcpy(&dsz, b + 0x14, 4);
            bool dataIsDDS = (size_t)doff + 4 < (size_t)r && b[doff] == 'D' && b[doff + 1] == 'D' &&
                             b[doff + 2] == 'S';
            // COPY the DDS out NOW (the buffer is freed once ER parses it). Only when the whole DDS
            // fits in THIS decompressed block (doff+dsz <= r) — small icon TPFs are self-contained;
            // multi-block sheets need block accumulation (not yet). Our copy is persistent → uploadable
            // anytime, no game bind, no read-after-free.
            if (dataIsDDS && (size_t)doff + dsz <= (size_t)r && dsz >= 148)
            {
                const uint8_t *d = b + doff;
                std::lock_guard<std::mutex> lk(g_tpf_mtx);
                if (g_dds_list.size() < 64)
                {
                    bool dup = false;
                    for (auto &e : g_dds_list)
                        if (e.size() == dsz && std::memcmp(e.data(), d, 32) == 0) { dup = true; break; }
                    if (!dup)
                        g_dds_list.emplace_back(d, d + dsz);
                }
            }
            static int n = 0;
            if (n < 16)
            {
                ++n;
                spdlog::info("[OODLE-TPF] @{:#x} sz={} files={} plat={} enc={} entry0(off={} size={} fmt={}) dataIsDDS={}",
                             reinterpret_cast<uintptr_t>(dst), r, fc, b[0x0C], b[0x0E], doff, dsz, b[0x18], dataIsDDS);
            }
        }
    }
    return r;
}

// IAT hook: overwrite eldenring.exe's import-table pointer for dll!fn with `detour`, saving the
// original into *orig. No trampoline/executable alloc (unlike MinHook → no MH_ERROR_MEMORY_ALLOC
// when oo2core sits >2GB away), and works for a cross-module import. Returns false if the module
// doesn't statically import dll!fn (e.g. it's resolved dynamically via GetProcAddress).
bool iat_hook(uintptr_t modBase, const char *dll, const char *fn, void *detour, void **orig)
{
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(modBase);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(modBase + dos->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;
    auto *desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(modBase + dir.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        const char *name = reinterpret_cast<const char *>(modBase + desc->Name);
        if (_stricmp(name, dll) != 0)
            continue;
        auto *thunk = reinterpret_cast<IMAGE_THUNK_DATA *>(modBase + desc->FirstThunk);
        DWORD origRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto *oThunk = reinterpret_cast<IMAGE_THUNK_DATA *>(modBase + origRva);
        for (; oThunk->u1.AddressOfData; ++thunk, ++oThunk)
        {
            if (oThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            auto *ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(modBase + oThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(ibn->Name), fn) != 0)
                continue;
            void **slot = reinterpret_cast<void **>(&thunk->u1.Function);
            DWORD oldp = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldp))
                return false;
            *orig = *slot;
            *slot = detour;
            VirtualProtect(slot, sizeof(void *), oldp, &oldp);
            return true;
        }
    }
    return false;
}

void install_oodle_hook()
{
    if (!goblin::config::dumpIconTextures || g_oodle_orig)
        return;
    uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!er)
        return;
    if (iat_hook(er, "oo2core_6_win64.dll", "OodleLZ_Decompress",
                 reinterpret_cast<void *>(&oodle_decompress_detour),
                 reinterpret_cast<void **>(&g_oodle_orig)))
        spdlog::info("[OODLE] IAT-hooked OodleLZ_Decompress (orig={}) — open the world map to log the TPF buffer",
                     reinterpret_cast<void *>(g_oodle_orig));
    else
        spdlog::warn("[OODLE] eldenring.exe has no static IAT import of oo2core!OodleLZ_Decompress "
                     "(dynamically loaded?) — decompress hook not installed");
}

// Browser over the distinct DDS captured from ER's decompresses (grabbed in the Oodle hook). count +
// fetch-by-index; the overlay uploads each into its own texture to find the icon sheet visually.
size_t goblin::tpf_dds_count()
{
    std::lock_guard<std::mutex> lk(g_tpf_mtx);
    return g_dds_list.size();
}

bool goblin::tpf_dds_at(size_t i, std::vector<uint8_t> &out)
{
    std::lock_guard<std::mutex> lk(g_tpf_mtx);
    if (i >= g_dds_list.size())
        return false;
    out = g_dds_list[i];
    return true;
}

// Map-point icon rect lookup (resolved from the resident image repo; see store_map_icon_rect, filled
// by the repo walk for MENU_MAP_*). Returns the SubTexture rect for a WORLD_MAP_POINT_PARAM.iconId on
// the SB_MapCursor[_ERR] sheet. false if not captured yet (open the world map) or the id is absent.
size_t goblin::map_icon_layout_count()
{
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    return g_map_icon_rects.size() + g_map_icon_named.size();
}

bool goblin::map_icon_rect(int iconId, int &x, int &y, int &w, int &h, void *&sheet)
{
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    auto it = g_map_icon_rects.find(iconId);
    if (it == g_map_icon_rects.end()) return false;
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

bool goblin::map_icon_rect_by_name(const char *name, int &x, int &y, int &w, int &h, void *&sheet)
{
    if (!name) return false;
    std::lock_guard<std::mutex> lk(g_map_icon_mtx);
    auto it = g_map_icon_named.find(name);
    if (it == g_map_icon_named.end())
    {
        // The GFx import/resolve formats "<symbol>_ptl" (the point-layout view), so the resident
        // sprite is registered under the _ptl name (e.g. MENU_MAP_ERR_Boss_ptl) while callers look
        // up the bare symbol (category_gpu_icon_name = "MENU_MAP_ERR_Boss"). Retry with the suffix so
        // the bare name still resolves — this was the gate hiding the boss (and other) map symbols.
        it = g_map_icon_named.find(std::string(name) + "_ptl");
        if (it == g_map_icon_named.end()) return false;
    }
    x = it->second.x; y = it->second.y; w = it->second.w; h = it->second.h; sheet = it->second.sheet;
    return true;
}

// Background harvest entry point — see goblin_inject.hpp. Relocates the proactive repo walk
// off the game-thread find hook onto the worldmap_probe background poll: harvest_repo_icons
// is read-only RPM on our own process and publishes under its mutexes, so it is thread-safe,
// and its internal throttle keeps the per-RPM Wine cost off the cadence of the engine thread.
void goblin::background_harvest_tick()
{
    harvest_repo_icons();   // resolves the repo from the static anchor; no-op unless icons wanted

    // Item-icon LAYOUT bootstrap (iconId→sheet+rect), HOISTED off the residency-ticker path. The disk
    // read+decompress of menu/hi/01_common.sblytbnd.dcx is pure file IO with NO dependency on the
    // CreateImage context or the menu-resource manager — so it must NOT wait for gpu_icon_tick, which
    // only runs once res_tick_detour captures the manager (an INVENTORY/menu event). A map-only session
    // never fires that, so every item icon stayed on the baked fallback (zero [ITEMLAYOUT], GAP#2 dead).
    // Kick it here on the worldmap-probe thread instead: runs from init, every 100ms, regardless of menu
    // state. Detached thread so the ~660KB decompress never hitches; a few spaced retries cover early
    // init before the mod dir resolves. load_item_icon_layout_from_disk() self-guards on count>0.
    if (goblin::config::nativeItemIcons)
    {
        static std::atomic<int> s_layout_tries{0};
        static std::atomic<bool> s_layout_inflight{false};
        if (goblin::item_icon_layout_count() == 0 && s_layout_tries.load() < 5 &&
            !s_layout_inflight.exchange(true))
        {
            s_layout_tries.fetch_add(1);
            std::thread([] {
                goblin::load_item_icon_layout_from_disk();
                s_layout_inflight.store(false);
            }).detach();
        }
    }
}

void goblin::install_icon_texture_probe()
{
    // The find/enum + residency-ticker + CreateImage hooks feed the PRODUCTION native-icon paths
    // (harvest_repo_icons → grace sprites + map_icon_rect rects), so install them whenever graces
    // or native item icons are on — NOT only under the dev dump flag (that was the gate that left
    // GPU graces/boss icons blank without dump_icon_textures). Their heavy work is internally
    // dump-gated; the light hook path + the throttled bulk-read walk are cheap enough for runtime.
    const bool dev = goblin::config::dumpIconTextures;
    const bool native = goblin::config::graceOverlay || goblin::config::nativeItemIcons;
    if (!dev && !native)
        return;
    // DEV-ONLY heavy machinery: Oodle DDS-capture hook + iconId calibration/verification.
    if (dev)
    {
        install_oodle_hook();
        goblin::verify_equip_iconids();
        self_calibrate_iconid();
    }
    void *fn = modutils::scan<void>({.aob = goblin::sig::WORLDMAP_CREATE_IMAGE});
    if (!fn)
    {
        spdlog::warn("[ICONTEX] CreateImage AOB not found (game patch?) — icon probe disabled");
        return;
    }
    try
    {
        modutils::hook(fn, reinterpret_cast<void *>(&create_image_detour),
                       reinterpret_cast<void **>(&g_create_image_orig));
        spdlog::info("[ICONTEX] CreateImage hooked @ {} (probe)", fn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[ICONTEX] hook failed: {}", e.what());
        g_create_image_orig = nullptr;
    }

    // Enumerate-loaded hook: capture the inventory movie ptr + reverse its image-list layout (safe
    // harvest path; sprite findings §1). Hooked by RVA (FUN_140d69640) — dev probe.
    try
    {
        uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        void *enumfn = reinterpret_cast<void *>(er + 0xd69640);
        modutils::hook(enumfn, reinterpret_cast<void *>(&enum_detour),
                       reinterpret_cast<void **>(&g_enum_orig));
        void *findfn = reinterpret_cast<void *>(er + 0xd63c30);
        modutils::hook(findfn, reinterpret_cast<void *>(&find_detour),
                       reinterpret_cast<void **>(&g_find_orig));
        spdlog::info("[ENUM2] find-hook (FUN_140d63c30) @ {} + enum @ {} — open inventory to harvest "
                     "resident item icons", findfn, enumfn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[ENUM] hook failed: {}", e.what());
        g_enum_orig = nullptr;
    }

    // Residency-ticker hook (findings §5e): capture the live menu-resource manager (*param_2) for the
    // bind-flip test + run queued one-shot actions inline on the engine thread. Idle cost = one deref
    // + one atomic load per tick. Dev-gated (whole probe is behind config::dumpIconTextures).
    try
    {
        uintptr_t er = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
        void *tickfn = reinterpret_cast<void *>(er + 0xd724c0);   // FUN_140d724c0
        modutils::hook(tickfn, reinterpret_cast<void *>(&res_tick_detour),
                       reinterpret_cast<void **>(&g_res_tick_orig));
        spdlog::info("[BINDTEST] residency-ticker hooked @ {} — open inventory/map to capture manager", tickfn);
    }
    catch (const std::exception &e)
    {
        spdlog::error("[BINDTEST] ticker hook failed: {}", e.what());
        g_res_tick_orig = nullptr;
    }
    // CreateImage context capture + force-replay (§5g) reuses the existing CreateImage probe hook
    // above (create_image_detour) — no separate hook (MinHook can't double-hook the same fn).
}

// Path A verify step (findings §6): on a MAP-OPEN frame, re-read each registered image's
// img+0x10 (the lazily-bound Scaleform::Render::Texture) → BFS to the GXTexture2D → +0x40 =
// ID3D12Resource. Confirms the load-time-null texture is now bound + reachable. Runs once
// per session after the chain first resolves. Called from the worldmap probe loop (map-open
// detector). Read-only RPM (reading the already-bound pointer is thread-safe; we do NOT call
// GetTexture, which would have to be on the render thread).
void goblin::dump_icon_textures_live()
{
    if (!goblin::config::dumpIconTextures || g_icon_imgs.empty())
        return;
    // REPEATABLE (throttled): images bind LAZILY + g_icon_imgs grows as the map draws more sprites,
    // so re-resolve periodically to accumulate ALL SB_ERR_*/icons (the old one-shot caught only what
    // was resident at the first run — e.g. a single grace frame). Candidate capture + logs dedup by
    // name; the verbose [ICONTEX-LIVE] summary stays one-shot (g_icon_live_done).
    static std::chrono::steady_clock::time_point s_last{};
    auto now = std::chrono::steady_clock::now();
    if (g_icon_live_done && now - s_last < std::chrono::milliseconds(500))
        return;
    s_last = now;
    uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("eldenring.exe"));
    if (!base)
        return;
    int bound = 0, resolved = 0, logged = 0;
    for (const IconImg &it : g_icon_imgs)
    {
        // ── Resolve the BOUND GPU sheet (binds LAZILY on first render). On vkd3d/Proton far fewer
        // sprites bind than on native D3D12, so this often fails on Linux even for valid sprites.
        // We DO NOT `continue` on failure: the candidate LIST is registered below GPU-independently
        // (fix: the list used to drop 3-4 candidates on Linux because only the 1-2 sprites that
        // happened to bind ever reached the push_back). Only the DISPLAY paths require sheet_ok.
        const uintptr_t mod_lo = base, mod_hi = base + 0x6000000;
        uintptr_t rtex = 0, res = 0, resvt = 0;
        int dim = 0, rw = 0, rh = 0, gfmt = 0;
        bool sheet_ok = false;
        // A REAL Scaleform Render::Texture = heap object (OUTSIDE the ER module) whose vtable is
        // INSIDE the module (.rdata). img+0x10 also takes junk states: 0 (unbound), a small int
        // (0x6), or a CODE/module pointer (0x141xxxxxx) — reject all three.
        if (icon_rpm_ptr(it.img + 0x10, rtex) && rtex >= 0x10000 && rtex < 0x7fffffffffffULL &&
            !(rtex >= mod_lo && rtex < mod_hi))
        {
            uintptr_t rtex_vt = 0;
            if (icon_rpm_ptr(rtex, rtex_vt) && rtex_vt >= mod_lo && rtex_vt < mod_hi)
            {
                ++bound;
                icon_detail_dump(rtex, base); // one-shot full Render::Texture layout (first bound)
                // rtex+0x70 = the backing ID3D12Resource. Do NOT gate on the RPM-read DIM at res+0x10:
                // the vkd3d resource layout drifted (a driver/game update; res+0x10 no longer reads 3) —
                // but res IS the resource; just validate a non-module vtable (a real COM object). The
                // overlay reads the authoritative format/dims via ID3D12Resource::GetDesc() at copy time.
                if (icon_rpm_ptr(rtex + 0x70, res) && res >= 0x10000 && res < 0x7fffffffffffULL &&
                    icon_rpm_ptr(res, resvt) && !(resvt >= mod_lo && resvt < mod_hi))
                {
                    icon_rpm_i32(res + 0x10, dim); icon_rpm_i32(res + 0x20, rw);
                    icon_rpm_i32(res + 0x28, rh); icon_rpm_i32(res + 0x30, gfmt);
                    ++resolved;
                    sheet_ok = true; // a bound, displayable GPU texture
                }
            }
        }
        // ERR map sprites (RE e4b3f6a §6): the discovered grace + all other ERR map icons draw through
        // CreateImage (not the find fn) → captured here with resolved sheet+rect. Collect EVERY
        // 'SB_ERR_*' candidate (dev viewer — to test whether non-grace icons harvest cleanly vs the
        // grace's time-of-day variants), and LOCK the grace sprite on the first LIT '..._Color' frame
        // with a grace-sized rect (any time-of-day; the engine resolves only one per session).
        if (it.name.rfind("SB_ERR_", 0) == 0 || it.name.rfind("MENU_MAP_", 0) == 0)
        {
            // The REAL grace = the mod's gfx sprite MENU_MAP_GOBLIN_Grace (force-created via img://);
            // SB_ERR_Grace_*_Color is the native time-tinted pin (wrong). Lock on the canonical; an
            // SB_ERR_Grace _Color frame is kept only as an overridable fallback. (MENU_MAP_ sprites have
            // no time-of-day variants, so they're always "lit".)
            // Canonical (default) grace = the vanilla icon MENU_MAP_01_Bonfire (bonfire = grace). The
            // ERR dungeon-style grace MENU_MAP_ERR_GraceUnderground is captured separately (below) and
            // used for dungeon graces when ERR is installed. GOBLIN_Grace/SB_ERR_Grace = F1 candidates.
            bool canon = it.name.rfind("MENU_MAP_01_Bonfire", 0) == 0;
            bool is_grace = canon || it.name.rfind("MENU_MAP_GOBLIN_Grace", 0) == 0 ||
                            it.name.rfind("SB_ERR_Grace", 0) == 0;
            bool lit = it.name.rfind("MENU_MAP_", 0) == 0 || it.name.find("Color") != std::string::npos;
            int gw = it.x1 - it.x0, gh = it.y1 - it.y0;
            bool rect_ok = gw >= 8 && gw <= 256 && gh >= 8 && gh <= 256;
            std::lock_guard<std::mutex> lk(g_harvest_mtx);
            // Candidate LIST is GPU-INDEPENDENT — register by name+rect whether or not the sheet is
            // bound, so vkd3d/Proton (which binds far fewer sprites) lists the SAME candidates as
            // native D3D12. Find-or-add by name; when the sheet later binds (sheet_ok), UPGRADE the
            // stored candidate in place so the F1 picker can actually DISPLAY it. (Sprites the game
            // never draws — e.g. the vanilla Bonfire on ERR — stay listed but never become displayable;
            // that's the inherent GPU limitation, not a list bug.)
            goblin::GraceCandidate *cand = nullptr;
            for (auto &gc : g_grace_cands) if (gc.name == it.name) { cand = &gc; break; }
            bool is_new = (cand == nullptr);
            if (is_new && g_grace_cands.size() < 64)
            {
                g_grace_cands.push_back(goblin::GraceCandidate{});
                cand = &g_grace_cands.back();
                cand->name = it.name;
                cand->spr.x0 = it.x0; cand->spr.y0 = it.y0; cand->spr.x1 = it.x1; cand->spr.y1 = it.y1;
                cand->spr.valid = false;   // listed-only until its texture binds
            }
            if (cand && sheet_ok && !cand->spr.valid)   // bound now → resolve sheet so it's displayable
            {
                cand->spr.sheet = reinterpret_cast<void *>(res);
                cand->spr.x0 = it.x0; cand->spr.y0 = it.y0; cand->spr.x1 = it.x1; cand->spr.y1 = it.y1;
                cand->spr.sheetW = static_cast<unsigned long long>(rw);
                cand->spr.sheetH = static_cast<unsigned>(rh);
                cand->spr.format = static_cast<unsigned>(gfmt);
                cand->spr.valid = true;
            }
            if (is_new)
                spdlog::info("[ICON-CAND] '{}' rect=({},{})-({},{}) res={:#x} fmt={} {}{}",
                             it.name, it.x0, it.y0, it.x1, it.y1, res, gfmt,
                             sheet_ok ? "bound" : "LISTED(unbound)", canon ? " <-CANONICAL grace" : "");
            // ── DISPLAY paths below require a BOUND sheet (only rendered sprites have a GPU texture
            // to copy). Keeping these gated on sheet_ok is what preserves the Windows display while the
            // list above is decoupled — the reverted attempt broke display by lazy-resolving these too.
            if (sheet_ok && is_grace && lit && rect_ok && !g_grace_locked && (canon || !g_grace_sprite.valid))
            {
                g_grace_sprite.sheet = reinterpret_cast<void *>(res);
                g_grace_sprite.x0 = it.x0; g_grace_sprite.y0 = it.y0;
                g_grace_sprite.x1 = it.x1; g_grace_sprite.y1 = it.y1;
                g_grace_sprite.sheetW = static_cast<unsigned long long>(rw);
                g_grace_sprite.sheetH = static_cast<unsigned>(rh);
                g_grace_sprite.format = static_cast<unsigned>(gfmt);
                g_grace_sprite.valid = true;
                if (canon) g_grace_locked = true;   // canonical wins + locks; fallback stays overridable
                spdlog::info("[GRACE-SPRITE] '{}' rect=({},{})-({},{}) {}x{} res={:#x} fmt={} ({})",
                             it.name, it.x0, it.y0, it.x1, it.y1, gw, gh, res, gfmt,
                             canon ? "LOCKED canonical MENU_MAP_01_Bonfire" : "fallback (awaiting canonical)");
            }
            // ERR dungeon-style grace — captured separately; its presence = ERR installed. The renderer
            // uses it for dungeon graces. Store once (it doesn't change).
            if (sheet_ok && it.name.rfind("MENU_MAP_ERR_GraceUnderground", 0) == 0 && rect_ok &&
                !g_grace_dungeon_sprite.valid)
            {
                g_grace_dungeon_sprite.sheet = reinterpret_cast<void *>(res);
                g_grace_dungeon_sprite.x0 = it.x0; g_grace_dungeon_sprite.y0 = it.y0;
                g_grace_dungeon_sprite.x1 = it.x1; g_grace_dungeon_sprite.y1 = it.y1;
                g_grace_dungeon_sprite.sheetW = static_cast<unsigned long long>(rw);
                g_grace_dungeon_sprite.sheetH = static_cast<unsigned>(rh);
                g_grace_dungeon_sprite.format = static_cast<unsigned>(gfmt);
                g_grace_dungeon_sprite.valid = true;
                spdlog::info("[GRACE-SPRITE] DUNGEON (ERR) '{}' rect=({},{})-({},{}) res={:#x} stored",
                             it.name, it.x0, it.y0, it.x1, it.y1, res);
            }
        }
        if (!sheet_ok)
            continue;   // unbound entry: candidate was listed above; the sheet tracking below needs res
        // Track unique sheet resources (icons on one sheet share a resource).
        bool known = false;
        for (uintptr_t s : g_icon_sheets) if (s == res) { known = true; break; }
        if (!known && g_icon_sheets.size() < 32)
        {
            g_icon_sheets.push_back(res);
            spdlog::info("[ICONTEX-LIVE] SHEET #{} res={:#x} resVt={:#x} {}x{} (DIM={})",
                         g_icon_sheets.size(), res, resvt, rw, rh, dim);
        }
        if (!g_icon_live_done && logged++ < 16)   // verbose only on the first pass (re-runs are quiet)
            spdlog::info("[ICONTEX-LIVE] name='{}' rect=({},{})-({},{}) sheet={}x{} | res={:#x} {}x{}",
                         it.name, it.x0, it.y0, it.x1, it.y1, it.w, it.h, res, rw, rh);
    }
    if (bound > 0)
    {
        spdlog::info("[ICONTEX-LIVE] summary: {}/{} bound, {} resolved via rtex+0x70, "
                     "{} unique sheet resources",
                     bound, g_icon_imgs.size(), resolved, g_icon_sheets.size());
        // Deterministic hop is solid once several images resolve to a small sheet set.
        if (resolved >= 5)
            g_icon_live_done = true;
    }
}

