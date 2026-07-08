#include "goblin_w2s.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "goblin_inject.hpp"   // get_player_world_pos
#include "goblin_warp.hpp"     // body_frame_origin — the render rebase origin, no scan

namespace
{
    // GameRendCameraSet@GameRend@CS@@ vtable RVA (this ERR build, imagebase 0x140000000).
    // The vtable qword sits at the GameRendCameraSet subobject = GameRend+0x10; the camera matrices are
    // at GameRend+0xF0 (VIEW) / +0x130 (identity in-live). See
    // docs/re/windows_world_to_screen_camera_re_findings.md.
    constexpr uintptr_t VT_CAMSET_RVA = 0x2a7f2b8;
    constexpr uintptr_t VIEW_FROM_HIT = 0xE0;   // GameRend+0xF0, hit = GameRend+0x10
    constexpr uintptr_t MB_FROM_HIT   = 0x120;  // GameRend+0x130 (the identity/second block)

    // ★ The REAL camera (2026-07-08, docs/re/windows_world_to_screen_camera_re_findings.md §the real
    // camera): GameRend+0xF0 turned out to be the CAMSRC POSE (its translation == the player BODY pos,
    // live-proven) — NOT the camera. The ACTIVE camera pose object hangs off GameRend+0x18:
    //   camMgr   = *(er+0x3d6b880)      (static slot — the camera step mgr FUN_140766980 passes to
    //                                    FUN_14076e7c0; scan-free)
    //   GameRend = *(camMgr+0x10)       (the SAME object the vtable scan finds)
    //   camObj   = *(GameRend+0x18)     (the active camera)
    //   camObj+0x10 = 4x4 POSE (rows = X/Y/Z camera axes + T position, cam->world, BODY frame)
    //   camObj+0x50 = lens {fovy(rad), aspect, near, far}  (live: 0.8727 / 1.7778 / 0.05 / 10000)
    constexpr uintptr_t CAM_MGR_SLOT_RVA    = 0x3d6b880;
    constexpr uintptr_t GAMEREND_FROM_MGR   = 0x10;
    constexpr uintptr_t CAMOBJ_FROM_GAMEREND = 0x18;
    constexpr uintptr_t POSE_FROM_CAMOBJ    = 0x10;
    constexpr uintptr_t LENS_FROM_CAMOBJ    = 0x50;

    // Live-tunable projection interpretation. Live calibration (2026-07-05, Fable 5, Linux) confirmed:
    // VIEW@GameRend+0xF0 is a clean row-vector view matrix, FOV=0.7505, conv=2 (row-vector, +Z fwd). BUT
    // the VIEW is in a render frame RE-CENTRED near the camera (ER rebases world coords for float
    // precision): its translation row is tiny (~(-2,5,4)) while the player is at ~(-58,92,99). So a GLOBAL
    // player pos must be REBASED (player - origin) before the VIEW. g_origin = that render origin.
    int   g_conv = 2;        // 0 rowvec+Zf, 1 rowmajor+Zf, 2 rowvec-Zf (confirmed), 3 rigid-inverse
    float g_fovy = 0.7505f;  // vertical FOV (radians) — confirmed from lens@GameRend+0x54
    bool  g_dot  = false;
    float g_origin[3] = {0, 0, 0};   // render rebase origin (world); subtracted before VIEW when g_have_origin
    bool  g_have_origin = false;
    long  g_origin_off = 0;          // offset of g_origin within GameRend where it was found (diagnostic)
    int   g_origin_src = 0;          // 0 = none, 1 = body-frame delta (exact), 2 = GameRend scan (fallback)

    // find_cam_instance scan diagnostics — surfaced by w2s_probe so the next boot MEASURES whether the
    // MEM_PRIVATE-restricted sweep now completes in the budget (vs the old 8 GB all-committed walk that
    // capped out mid-sweep). Reset when a fresh sweep starts; g_scan_done = a full sweep finished this call.
    size_t g_scan_bytes = 0;
    bool   g_scan_done = false;

    __declspec(noinline) bool safe_read(const void *src, void *dst, size_t n)
    {
        __try { std::memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    template <class T> bool rd(uintptr_t a, T &out) { return safe_read((const void *)a, &out, sizeof(T)); }

    bool finite16(const float *m)
    {
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e6f) return false;
        return true;
    }
    // A plausible VIEW matrix: orthonormal-ish upper 3x3 + 4th column (0,0,0,1), not identity.
    bool looks_like_view(const float *m)
    {
        if (!finite16(m)) return false;
        if (std::fabs(m[3]) > 1e-3f || std::fabs(m[7]) > 1e-3f || std::fabs(m[11]) > 1e-3f) return false;
        if (std::fabs(m[15] - 1.f) > 1e-3f) return false;
        auto rowlen = [&](int r) { return std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]); };
        for (int r = 0; r < 3; ++r)
            if (std::fabs(rowlen(r) - 1.f) > 0.05f) return false;
        // reject identity (translation zero AND rotation identity)
        bool ident = std::fabs(m[0] - 1) < 1e-3f && std::fabs(m[5] - 1) < 1e-3f && std::fabs(m[10] - 1) < 1e-3f &&
                     std::fabs(m[12]) < 1e-3f && std::fabs(m[13]) < 1e-3f && std::fabs(m[14]) < 1e-3f;
        return !ident;
    }

    // Crash-safe scan for the GameRendCameraSet vtable -> the camera instance whose +0xE0 is a valid
    // VIEW matrix. Cached; re-validated (vtable qword still present) each call.
    //
    // ★ Present-thread-SAFE (Windows-hardening 2026-07-07, docs/re/windows_w2s_camera_finder_present_hang):
    // GameRend is NOT an FD4Singleton — it has no static slot (it is task-tree-resident, referenced as
    // renderObj+0xE8 / InGameStep+0xB3628 by many owners; RE'd in Ghidra). So there is no "one deref off a
    // static" path; finding the live instance genuinely needs a memory scan. The OLD scan walked the WHOLE
    // multi-GB committed space in one call on the PRESENT thread — a single pass froze the frame (the greybox
    // render hang). The fix is to TIME-BOX the scan and RESUME across frames via a persistent cursor: each
    // per-frame call scans for at most `budget_ms`, then returns 0 (no camera THIS frame — render just skips)
    // and picks up where it left off next frame. Once a hit is cached, every later frame is the cheap
    // re-validate path. A permanently-absent/rejected vtable degrades to "no render" (a light periodic
    // sweep), never a hang. `exhaustive` = force a FRESH full sweep in one call (RPC `w2s_probe` wants a
    // definitive answer, not a resumed slice) with a 2 s hard cap so even that can't wedge. Present-thread
    // only (single caller set: probe / draw_present / get_camera) → the file-static scan state needs no lock.
    uintptr_t find_cam_instance(bool exhaustive = false)
    {
        static uintptr_t s_hit = 0;
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return 0;
        uintptr_t vt = base + VT_CAMSET_RVA;

        if (s_hit)
        {
            uintptr_t q = 0; float m[16];
            if (rd(s_hit, q) && q == vt && rd(s_hit + VIEW_FROM_HIT, m) && looks_like_view(m))
                return s_hit;
            s_hit = 0;  // stale — rescan
        }

        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t amin = (uintptr_t)si.lpMinimumApplicationAddress;
        uintptr_t aend = (uintptr_t)si.lpMaximumApplicationAddress;
        const size_t WIN = 8 * 1024 * 1024, OVL = 0x200;

        // Cross-frame scan state: cursor = absolute resume address; a completed miss backs off for
        // kCooldownMs so a never-present vtable doesn't re-sweep every single frame.
        static std::vector<uint8_t> s_buf;
        static uintptr_t s_cursor = 0;
        static bool s_sweeping = false;
        static DWORD s_next_sweep_tick = 0;
        if (s_buf.size() < WIN + OVL) s_buf.resize(WIN + OVL);

        // Per-CALL time box: tiny for the per-frame render path (frame-safe, resumes next frame); generous
        // for the manual RPC probe so ONE `w2s_probe` usually completes a full sweep, still capped so it
        // can't wedge on a pathologically large region (it just resumes on the next `w2s_probe`).
        const DWORD budget_ms = exhaustive ? 2000 : 2;
        const DWORD kCooldownMs = 2000;                  // wait between full sweeps that found nothing
        const DWORD t0 = GetTickCount();

        if (exhaustive)
        {
            s_sweeping = true; s_cursor = amin; g_scan_bytes = 0; g_scan_done = false;   // fresh complete sweep
        }
        else if (!s_sweeping)
        {
            if ((int)(s_next_sweep_tick - t0) > 0) return 0;   // still cooling down
            s_sweeping = true; s_cursor = amin; g_scan_bytes = 0; g_scan_done = false;
        }

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t addr = s_cursor;
        while (addr < aend && VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            uintptr_t rbase = (uintptr_t)mbi.BaseAddress; size_t rsz = mbi.RegionSize; DWORD pr = mbi.Protect;
            // The camera instance is a HEAP object (FD4 allocator) → MEM_PRIVATE, read/write, NON-exec.
            // Restricting to that skips the loaded-module IMAGES and file/texture MAPPINGS — the bulk of ER's
            // multi-GB footprint — so a full sweep covers far less and can finish in the budget (the old
            // all-committed walk capped out mid-sweep on this ~8 GB process and never resolved the camera).
            bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rsz >= 16 &&
                      (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
                      !(pr & (PAGE_GUARD | PAGE_NOACCESS));
            // Resume mid-region: skip windows already scanned this sweep (cursor landed inside rbase..+rsz).
            size_t start_off = (s_cursor > rbase && s_cursor < rbase + rsz) ? ((s_cursor - rbase) & ~(WIN - 1)) : 0;
            if (ok)
                for (size_t off = start_off; off < rsz; off += WIN)
                {
                    size_t want = rsz - off; if (want > WIN + OVL) want = WIN + OVL;
                    if (safe_read((void *)(rbase + off), s_buf.data(), want))
                    {
                        g_scan_bytes += want;
                        size_t scan_to = (want > WIN) ? WIN : (want >= 8 ? want - 8 : 0);
                        for (size_t i = 0; i < scan_to; i += 8)
                            if (*(const uint64_t *)(s_buf.data() + i) == vt)
                            {
                                uintptr_t hit = rbase + off + i; float m[16];
                                if (rd(hit + VIEW_FROM_HIT, m) && looks_like_view(m))
                                { s_hit = hit; s_sweeping = false; s_cursor = 0; g_scan_done = true; return hit; }
                            }
                    }
                    if ((GetTickCount() - t0) >= budget_ms)
                    { s_cursor = rbase + off + WIN; return 0; }   // out of time — resume here next call
                }
            uintptr_t nxt = rbase + rsz; if (nxt <= addr) break; addr = nxt;
            if ((GetTickCount() - t0) >= budget_ms) { s_cursor = addr; return 0; }
        }
        // Full sweep completed with no valid hit — reset + back off before the next sweep.
        s_sweeping = false; s_cursor = 0; g_scan_done = true; s_next_sweep_tick = GetTickCount() + kCooldownMs;
        return 0;
    }

    // Transform world point by VIEW under interpretation `conv`; out = (vx,vy,vz,forward). forward>0 = in front.
    void to_view(const float *m, float x, float y, float z, int conv, float &vx, float &vy, float &vz, float &fwd)
    {
        if (conv == 1)  // row-major M*v
        {
            vx = m[0] * x + m[1] * y + m[2] * z + m[3];
            vy = m[4] * x + m[5] * y + m[6] * z + m[7];
            vz = m[8] * x + m[9] * y + m[10] * z + m[11];
        }
        else if (conv == 3)  // treat M as camera->world rigid: view = R^T*(p - t)
        {
            float dx = x - m[12], dy = y - m[13], dz = z - m[14];
            vx = m[0] * dx + m[1] * dy + m[2] * dz;   // R rows dotted (R^T * d)
            vy = m[4] * dx + m[5] * dy + m[6] * dz;
            vz = m[8] * dx + m[9] * dy + m[10] * dz;
        }
        else  // 0 or 2: row-vector v*M, with the render rebase applied (player - origin) before the VIEW
        {
            float rx = x, ry = y, rz = z;
            if (g_have_origin) { rx -= g_origin[0]; ry -= g_origin[1]; rz -= g_origin[2]; }
            vx = m[0] * rx + m[4] * ry + m[8] * rz + m[12];
            vy = m[1] * rx + m[5] * ry + m[9] * rz + m[13];
            vz = m[2] * rx + m[6] * ry + m[10] * rz + m[14];
        }
        fwd = (conv == 2) ? -vz : vz;
    }

    // Pinhole projection of a view-space point -> screen px (square pixels, symmetric frustum).
    bool project(const float *m, float x, float y, float z, int conv, float fovy, float W, float H, float &sx, float &sy)
    {
        float vx, vy, vz, fwd; to_view(m, x, y, z, conv, vx, vy, vz, fwd);
        if (!(fwd > 0.01f)) return false;
        float focal = (H * 0.5f) / std::tan(fovy * 0.5f);
        sx = W * 0.5f + (vx / fwd) * focal;
        sy = H * 0.5f - (vy / fwd) * focal;   // screen Y down
        return true;
    }

    // Scan the GameRend struct for the render REBASE ORIGIN: the float3 O such that projecting
    // (player - O) through the VIEW (conv2) centres the player on screen. Same-frame (no tearing).
    // Discriminated with a 2nd point (player + 3m up) so a coincidental float3 doesn't win. On success
    // sets g_origin/g_have_origin/g_origin_off and returns true. `gr` = GameRend base, `m` = VIEW.
    bool find_origin(uintptr_t gr, const float *m, float px, float py, float pz, float W, float H)
    {
        const long LO = -0x40, HI = 0x600;
        std::vector<uint8_t> buf((size_t)(HI - LO));
        if (!safe_read((const void *)(gr + LO), buf.data(), buf.size())) return false;
        float best = 1e9f; long best_off = 0; float bo[3] = {0, 0, 0};
        bool save_have = g_have_origin; float save_o[3] = {g_origin[0], g_origin[1], g_origin[2]};
        g_have_origin = true;
        for (long o = 0; o + 12 <= (long)buf.size(); o += 4)
        {
            const float *c = (const float *)(buf.data() + o);
            if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) continue;
            // render origin sits near the active region: within a few km of the player, not at 0/huge
            if (std::fabs(px - c[0]) > 3000 || std::fabs(py - c[1]) > 3000 || std::fabs(pz - c[2]) > 3000) continue;
            g_origin[0] = c[0]; g_origin[1] = c[1]; g_origin[2] = c[2];
            float sx, sy, sx2, sy2;
            if (!project(m, px, py, pz, 2, g_fovy, W, H, sx, sy)) continue;
            if (sx < 0 || sx > W || sy < 0 || sy > H) continue;                 // player on screen
            if (!project(m, px, py + 3.f, pz, 2, g_fovy, W, H, sx2, sy2)) continue;
            if (std::fabs(sx2 - sx) > 0.15f * W) continue;                       // head ~above, small dx
            if (!(sy2 < sy - 1.f)) continue;                                     // head strictly higher on screen
            float score = std::fabs(sx - W * 0.5f) + std::fabs(sy - H * 0.5f);   // prefer centred
            if (score < best) { best = score; best_off = LO + o; bo[0] = c[0]; bo[1] = c[1]; bo[2] = c[2]; }
        }
        if (best < 1e9f)
        {
            g_origin[0] = bo[0]; g_origin[1] = bo[1]; g_origin[2] = bo[2]; g_origin_off = best_off;
            g_have_origin = true;
            return true;
        }
        g_have_origin = save_have; g_origin[0] = save_o[0]; g_origin[1] = save_o[1]; g_origin[2] = save_o[2];
        return false;
    }

    // Read the ACTIVE camera POSE + lens and build the row-vector -Z-forward VIEW — the drop-in
    // replacement for the old GameRend+0xF0 read (which was the camsrc/player pose, not the camera).
    // Chain: static slot er+0x3d6b880 first (scan-free); fallback = the MEM_PRIVATE vtable scan's
    // GameRend. The pose is cam->world in the BODY frame (rebase world points by the body-frame
    // origin first — resolve_origin). VIEW = rigid inverse with the Z column NEGATED so the existing
    // conv2 (-Z forward) projection + r3d perspNegZ pipeline work unchanged:
    //   v·M: vx=(b-T)·X  vy=(b-T)·Y  vz=-(b-T)·Z  → fwd=-vz=(b-T)·Z.
    bool read_camera_view(float outView[16], float &outFovy, uintptr_t *outGameRend = nullptr,
                          uintptr_t *outCamObj = nullptr, float *outPose16 = nullptr,
                          float *outLens4 = nullptr)
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return false;
        uintptr_t gameRend = 0, mgr = 0;
        if (rd(base + CAM_MGR_SLOT_RVA, mgr) && mgr > 0x10000)
        {
            uintptr_t gr = 0;
            if (rd(mgr + GAMEREND_FROM_MGR, gr) && gr > 0x10000) gameRend = gr;
        }
        if (!gameRend)
        {
            uintptr_t hit = find_cam_instance();
            if (hit) gameRend = hit - 0x10;
        }
        if (!gameRend) return false;
        if (outGameRend) *outGameRend = gameRend;
        uintptr_t camObj = 0;
        if (!rd(gameRend + CAMOBJ_FROM_GAMEREND, camObj) || camObj < 0x10000) return false;
        if (outCamObj) *outCamObj = camObj;
        float pose[16];
        if (!rd(camObj + POSE_FROM_CAMOBJ, pose) || !finite16(pose)) return false;
        if (outPose16) std::memcpy(outPose16, pose, sizeof(pose));
        // pose sanity: rows 0..2 are unit axes, 4th column (0,0,0,1)
        auto rowlen = [&](int r) {
            return std::sqrt(pose[r * 4] * pose[r * 4] + pose[r * 4 + 1] * pose[r * 4 + 1] +
                             pose[r * 4 + 2] * pose[r * 4 + 2]);
        };
        for (int r = 0; r < 3; ++r)
            if (std::fabs(rowlen(r) - 1.f) > 0.05f) return false;
        if (std::fabs(pose[15] - 1.f) > 1e-3f) return false;
        const float *X = pose + 0, *Y = pose + 4, *Z = pose + 8, *T = pose + 12;
        float M[16] = {
            X[0], Y[0], -Z[0], 0.f,
            X[1], Y[1], -Z[1], 0.f,
            X[2], Y[2], -Z[2], 0.f,
            -(T[0] * X[0] + T[1] * X[1] + T[2] * X[2]),
            -(T[0] * Y[0] + T[1] * Y[1] + T[2] * Y[2]),
            +(T[0] * Z[0] + T[1] * Z[1] + T[2] * Z[2]), 1.f};
        std::memcpy(outView, M, sizeof(M));
        float lens[4] = {0, 0, 0, 0};
        bool lens_ok = rd(camObj + LENS_FROM_CAMOBJ, lens);
        if (outLens4) std::memcpy(outLens4, lens, sizeof(lens));
        outFovy = (lens_ok && lens[0] > 0.1f && lens[0] < 2.6f) ? lens[0] : g_fovy;
        return true;
    }

    // Resolve the render rebase origin. PRIMARY = the body-frame delta (goblin::warp::
    // body_frame_origin — tile(+0x6C0) − body(posObj+0x70)): the engine builds the VIEW from that
    // SAME havok pose (FUN_1403f0f60→FUN_14045e540 reads [[camsrc+0x190]+0x68]), so the VIEW's
    // frame IS the body frame and the origin is exact by construction — no scan. Accepted when the
    // player projects in FRONT of the camera (fwd>0, the cheap sanity for a torn read mid-load).
    // FALLBACK = the old GameRend float3 scan heuristic (it picked (0,2,2000) on 2.6.2.0 — wrong,
    // hence the off-position boxes — but it is kept for a build where the body chain shifts).
    bool resolve_origin(uintptr_t gr, const float *m, float px, float py, float pz, float W, float H)
    {
        float o[3];
        if (goblin::warp::body_frame_origin(o[0], o[1], o[2]))
        {
            float save_o[3] = {g_origin[0], g_origin[1], g_origin[2]};
            bool save_have = g_have_origin;
            g_origin[0] = o[0]; g_origin[1] = o[1]; g_origin[2] = o[2];
            g_have_origin = true;
            float vx, vy, vz, fwd;
            to_view(m, px, py, pz, 2, vx, vy, vz, fwd);
            if (fwd > 0.01f)
            {
                g_origin_src = 1; g_origin_off = 0;
                return true;
            }
            g_have_origin = save_have;
            g_origin[0] = save_o[0]; g_origin[1] = save_o[1]; g_origin[2] = save_o[2];
        }
        if (find_origin(gr, m, px, py, pz, W, H)) { g_origin_src = 2; return true; }
        g_origin_src = 0;
        return false;
    }
}

namespace goblin::w2s
{
    void set_debug_dot(bool on) { g_dot = on; }

    std::string probe()
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return "err: eldenring.exe base not found";
        float px, py, pz;
        if (!goblin::get_player_world_pos(px, py, pz)) return "err: no player world pos (in-world?)";
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        float W = ds.x > 0 ? ds.x : 1920.f, H = ds.y > 0 ? ds.y : 1080.f;

        float m[16], pose[16] = {}, lens[4] = {}, fovy = g_fovy;
        uintptr_t gameRend = 0, camObj = 0;
        if (!read_camera_view(m, fovy, &gameRend, &camObj, pose, lens))
        {
            // static chain dead — run the exhaustive scan for the coverage diagnostic
            uintptr_t hit = find_cam_instance(/*exhaustive=*/true);
            char eb[288];
            std::snprintf(eb, sizeof(eb),
                          "err: camera unresolved — static chain (er+0x3d6b880) dead AND vtable sweep %s "
                          "after %.0f MB (MEM_PRIVATE), hit=%#llx (in-world? camera active?)",
                          g_scan_done ? "COMPLETED" : "timed out mid-sweep",
                          g_scan_bytes / (1024.0 * 1024.0), (unsigned long long)hit);
            return eb;
        }

        std::string out;
        char b[512];
        std::snprintf(b, sizeof(b),
                      "ok w2s cam chain er+0x3d6b880 -> GameRend=%#llx -> camObj=%#llx player=(%.2f,%.2f,%.2f) view=%.0fx%.0f dot=%d\n",
                      (unsigned long long)gameRend, (unsigned long long)camObj, px, py, pz, W, H, (int)g_dot);
        out += b;
        std::snprintf(b, sizeof(b),
                      "CAMERA pose@camObj+0x10: X=[%.4f %.4f %.4f] Y=[%.4f %.4f %.4f] Z=[%.4f %.4f %.4f] T=(%.2f,%.2f,%.2f)\n"
                      "lens@+0x50: fovy=%.5f aspect=%.5f near=%.3f far=%.0f (using fovy=%.5f)\n",
                      pose[0], pose[1], pose[2], pose[4], pose[5], pose[6], pose[8], pose[9], pose[10],
                      pose[12], pose[13], pose[14], lens[0], lens[1], lens[2], lens[3], fovy);
        out += b;
        // the old +0xF0 block, kept for the record: it is the CAMSRC (player/freecam) POSE, not the camera
        float f0[16] = {};
        if (rd(gameRend + 0xF0, f0))
        {
            std::snprintf(b, sizeof(b), "camsrc-pose@GameRend+0xF0 trans=(%.2f,%.2f,%.2f) (player body — NOT the camera)\n",
                          f0[12], f0[13], f0[14]);
            out += b;
        }
        {
            float o[3];
            if (goblin::warp::body_frame_origin(o[0], o[1], o[2]))
            {
                std::snprintf(b, sizeof(b), "BODY-FRAME origin=(%.2f,%.2f,%.2f) (tile+0x6C0 - body+0x70)\n",
                              o[0], o[1], o[2]);
                out += b;
            }
            else
                out += "BODY-FRAME origin: unresolved (chain null — mid-load?)\n";
        }
        // resolve as the render would (body-frame primary, scan fallback) and project the player
        if (resolve_origin(gameRend, m, px, py, pz, W, H))
        {
            float sx, sy;
            bool vis = project(m, px, py, pz, 2, fovy, W, H, sx, sy);
            float sx2, sy2;
            bool vis2 = project(m, px, py + 1.7f, pz, 2, fovy, W, H, sx2, sy2);
            char srcbuf[40];
            if (g_origin_src == 1) std::snprintf(srcbuf, sizeof(srcbuf), "body-frame");
            else std::snprintf(srcbuf, sizeof(srcbuf), "GameRend-scan@%+ld", g_origin_off);
            std::snprintf(b, sizeof(b),
                          "REBASE origin=(%.2f,%.2f,%.2f) src=%s -> player FEET px=(%.0f,%.0f)%s HEAD px=(%.0f,%.0f)%s <== w2s3d\n",
                          g_origin[0], g_origin[1], g_origin[2], srcbuf,
                          sx, sy, vis ? "" : "(behind)", sx2, sy2, vis2 ? "" : "(behind)");
            out += b;
        }
        else
            out += "REBASE origin: NOT FOUND (body chain null + GameRend scan miss)\n";
        spdlog::info("[W2S] {}", out);
        return out;
    }

    void draw_present()
    {
        if (!g_dot) return;
        float m[16], fovy = g_fovy;
        uintptr_t gameRend = 0;
        if (!read_camera_view(m, fovy, &gameRend)) return;
        float px, py, pz;
        if (!goblin::get_player_world_pos(px, py, pz)) return;
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        float W = ds.x, H = ds.y; if (!(W > 0 && H > 0)) return;
        // ER renders camera-relative: rebase the global player pos before the VIEW. Re-resolve the origin
        // each frame (cheap, same-frame) so it tracks as the render frame re-centres while the player moves.
        resolve_origin(gameRend, m, px, py, pz, W, H);
        float sx, sy;
        if (!project(m, px, py, pz, g_conv, fovy, W, H, sx, sy)) return;
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        dl->AddCircle(ImVec2(sx, sy), 10.f, IM_COL32(255, 40, 40, 255), 16, 2.5f);
        dl->AddLine(ImVec2(sx - 14, sy), ImVec2(sx + 14, sy), IM_COL32(255, 40, 40, 200), 1.f);
        dl->AddLine(ImVec2(sx, sy - 14), ImVec2(sx, sy + 14), IM_COL32(255, 40, 40, 200), 1.f);
    }

    // exposed for the RPC verb to tweak convention/fov live
    void set_conv(int c) { g_conv = c; }
    void set_fovy(float f) { g_fovy = f; }

    // Public world->screen for the ImGui object-box render (uses g_origin set by the last get_camera).
    bool project_world(const float view[16], float fovy, float x, float y, float z,
                       float W, float H, float &sx, float &sy)
    {
        return project(view, x, y, z, 2 /*conv2 forward=-vz*/, fovy, W, H, sx, sy);
    }

    // Live ER camera for the 3D backend (goblin_r3d): the REAL camera VIEW (built from the active
    // camera POSE at [[[er+0x3d6b880]+0x10]+0x18]+0x10 — row-vector v*M, conv2 forward=-vz, drop-in
    // for the old +0xF0 read which was the camsrc pose) + the per-frame REBASE origin (body-frame
    // delta; subtract from a world point before the VIEW) + the LIVE vertical FOV (lens@camObj+0x50).
    // Re-resolves everything each call (cheap: 4 guarded reads). false if the camera/origin can't be
    // resolved (menu / not in-world). Present-thread only.
    bool get_camera(float outView[16], float outOrigin[3], float &outFovy, float vpW, float vpH)
    {
        float m[16], fovy = g_fovy;
        uintptr_t gameRend = 0;
        if (!read_camera_view(m, fovy, &gameRend)) return false;
        float px, py, pz;
        if (!goblin::get_player_world_pos(px, py, pz)) return false;
        if (!(vpW > 0 && vpH > 0)) return false;
        if (!resolve_origin(gameRend, m, px, py, pz, vpW, vpH)) return false;
        static int s_logged_src = -1;
        if (g_origin_src != s_logged_src)
        {
            s_logged_src = g_origin_src;
            spdlog::info("[W2S] origin source -> {} origin=({:.2f},{:.2f},{:.2f}) fovy={:.4f}",
                         g_origin_src == 1 ? "body-frame (exact)" : "GameRend scan (fallback)",
                         g_origin[0], g_origin[1], g_origin[2], fovy);
        }
        for (int i = 0; i < 16; ++i) outView[i] = m[i];
        outOrigin[0] = g_origin[0]; outOrigin[1] = g_origin[1]; outOrigin[2] = g_origin[2];
        outFovy = fovy;
        return true;
    }
}
