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

namespace
{
    // GameRendCameraSet@GameRend@CS@@ vtable RVA (this ERR build, imagebase 0x140000000).
    // The vtable qword sits at the GameRendCameraSet subobject = GameRend+0x10; the camera matrices are
    // at GameRend+0xF0 (VIEW) / +0x130 (identity in-live). See
    // docs/re/windows_world_to_screen_camera_re_findings.md.
    constexpr uintptr_t VT_CAMSET_RVA = 0x2a7f2b8;
    constexpr uintptr_t VIEW_FROM_HIT = 0xE0;   // GameRend+0xF0, hit = GameRend+0x10
    constexpr uintptr_t MB_FROM_HIT   = 0x120;  // GameRend+0x130 (the identity/second block)

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
            s_sweeping = true; s_cursor = amin;   // manual probe: always a FRESH complete sweep
        }
        else if (!s_sweeping)
        {
            if ((int)(s_next_sweep_tick - t0) > 0) return 0;   // still cooling down
            s_sweeping = true; s_cursor = amin;
        }

        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t addr = s_cursor;
        while (addr < aend && VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            uintptr_t rbase = (uintptr_t)mbi.BaseAddress; size_t rsz = mbi.RegionSize; DWORD pr = mbi.Protect;
            bool ok = mbi.State == MEM_COMMIT && rsz >= 16 &&
                      (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                      !(pr & (PAGE_GUARD | PAGE_NOACCESS));
            // Resume mid-region: skip windows already scanned this sweep (cursor landed inside rbase..+rsz).
            size_t start_off = (s_cursor > rbase && s_cursor < rbase + rsz) ? ((s_cursor - rbase) & ~(WIN - 1)) : 0;
            if (ok)
                for (size_t off = start_off; off < rsz; off += WIN)
                {
                    size_t want = rsz - off; if (want > WIN + OVL) want = WIN + OVL;
                    if (safe_read((void *)(rbase + off), s_buf.data(), want))
                    {
                        size_t scan_to = (want > WIN) ? WIN : (want >= 8 ? want - 8 : 0);
                        for (size_t i = 0; i < scan_to; i += 8)
                            if (*(const uint64_t *)(s_buf.data() + i) == vt)
                            {
                                uintptr_t hit = rbase + off + i; float m[16];
                                if (rd(hit + VIEW_FROM_HIT, m) && looks_like_view(m))
                                { s_hit = hit; s_sweeping = false; s_cursor = 0; return hit; }
                            }
                    }
                    if ((GetTickCount() - t0) >= budget_ms)
                    { s_cursor = rbase + off + WIN; return 0; }   // out of time — resume here next call
                }
            uintptr_t nxt = rbase + rsz; if (nxt <= addr) break; addr = nxt;
            if ((GetTickCount() - t0) >= budget_ms) { s_cursor = addr; return 0; }
        }
        // Full sweep completed with no valid hit — reset + back off before the next sweep.
        s_sweeping = false; s_cursor = 0; s_next_sweep_tick = GetTickCount() + kCooldownMs;
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
}

namespace goblin::w2s
{
    void set_debug_dot(bool on) { g_dot = on; }

    std::string probe()
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return "err: eldenring.exe base not found";
        uintptr_t hit = find_cam_instance(/*exhaustive=*/true);   // manual probe wants a full sweep, not a slice
        if (!hit) return "err: no GameRendCameraSet instance with a valid VIEW matrix (in-world? camera active? "
                         "run w2s_probe again if it timed out mid-sweep)";

        float px, py, pz;
        if (!goblin::get_player_world_pos(px, py, pz)) return "err: no player world pos (in-world?)";

        float m[16] = {}, m2[16] = {}, lens[8] = {};
        rd(hit + VIEW_FROM_HIT, m);
        rd(hit + MB_FROM_HIT, m2);
        rd(hit - 0x10 + 0x50, lens);   // GameRend+0x50.. lens scalars (fov/near/far candidates)

        ImVec2 ds = ImGui::GetIO().DisplaySize;
        float W = ds.x > 0 ? ds.x : 1920.f, H = ds.y > 0 ? ds.y : 1080.f;

        std::string out;
        char b[512];
        std::snprintf(b, sizeof(b), "ok w2s cam GameRend=%#llx player=(%.2f,%.2f,%.2f) view=%.0fx%.0f conv=%d fovy=%.4f dot=%d\n",
                      (unsigned long long)(hit - 0x10), px, py, pz, W, H, g_conv, g_fovy, (int)g_dot);
        out += b;
        std::snprintf(b, sizeof(b), "VIEW@+0xF0:\n [%.4f %.4f %.4f %.4f]\n [%.4f %.4f %.4f %.4f]\n [%.4f %.4f %.4f %.4f]\n [%.4f %.4f %.4f %.4f]\n",
                      m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        out += b;
        std::snprintf(b, sizeof(b), "+0x130: [%.3f %.3f %.3f %.3f ...]  lens@+0x50: %.4f %.4f %.4f %.4f %.4f %.4f\n",
                      m2[0], m2[1], m2[2], m2[3], lens[0], lens[1], lens[2], lens[3], lens[4], lens[5]);
        out += b;
        // report every interpretation WITHOUT rebase (origin off) — shows the raw off-screen result
        bool save_ho = g_have_origin; g_have_origin = false;
        for (int c = 0; c < 4; ++c)
        {
            float vx, vy, vz, fwd; to_view(m, px, py, pz, c, vx, vy, vz, fwd);
            float sx = -1, sy = -1; bool vis = project(m, px, py, pz, c, g_fovy, W, H, sx, sy);
            std::snprintf(b, sizeof(b), " conv%d(no-rebase): view=(%.2f,%.2f,%.2f) fwd=%.2f -> px=(%.0f,%.0f) %s\n",
                          c, vx, vy, vz, fwd, sx, sy, vis ? "" : "(behind)");
            out += b;
        }
        g_have_origin = save_ho;
        // auto-find the render rebase origin, then project conv2 WITH it (the real answer)
        if (find_origin(hit - 0x10, m, px, py, pz, W, H))
        {
            float sx, sy; bool vis = project(m, px, py, pz, 2, g_fovy, W, H, sx, sy);
            std::snprintf(b, sizeof(b), "REBASE origin=(%.2f,%.2f,%.2f) @GameRend%+ld -> conv2 px=(%.0f,%.0f) %s <== w2s3d\n",
                          g_origin[0], g_origin[1], g_origin[2], g_origin_off, sx, sy, vis ? "LOCKED" : "?");
            out += b;
        }
        else
            out += "REBASE origin: NOT FOUND in GameRend[-0x40..+0x600] — widen the window or the origin is elsewhere\n";
        spdlog::info("[W2S] {}", out);
        return out;
    }

    void draw_present()
    {
        if (!g_dot) return;
        uintptr_t hit = find_cam_instance();
        if (!hit) return;
        float px, py, pz;
        if (!goblin::get_player_world_pos(px, py, pz)) return;
        float m[16];
        if (!rd(hit + VIEW_FROM_HIT, m) || !looks_like_view(m)) return;
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        float W = ds.x, H = ds.y; if (!(W > 0 && H > 0)) return;
        // ER renders camera-relative: rebase the global player pos before the VIEW. Re-find the origin
        // each frame (cheap, same-frame) so it tracks as the render frame re-centres while the player moves.
        find_origin(hit - 0x10, m, px, py, pz, W, H);
        float sx, sy;
        if (!project(m, px, py, pz, g_conv, g_fovy, W, H, sx, sy)) return;
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

    // Live ER camera for the 3D backend (goblin_r3d): the render-local VIEW matrix (GameRend+0xF0, row-vector
    // v*M, conv2 forward=-vz) + the per-frame REBASE origin (subtract from a world point before the VIEW) +
    // the vertical FOV. Re-finds the origin each call (the render re-centres per frame). false if the camera/
    // origin can't be resolved (menu / not in-world). Present-thread only.
    bool get_camera(float outView[16], float outOrigin[3], float &outFovy, float vpW, float vpH)
    {
        uintptr_t hit = find_cam_instance();
        if (!hit) return false;
        float m[16];
        if (!rd(hit + VIEW_FROM_HIT, m) || !looks_like_view(m)) return false;
        float px, py, pz;
        if (!goblin::get_player_world_pos(px, py, pz)) return false;
        if (!(vpW > 0 && vpH > 0)) return false;
        if (!find_origin(hit - 0x10, m, px, py, pz, vpW, vpH)) return false;
        for (int i = 0; i < 16; ++i) outView[i] = m[i];
        outOrigin[0] = g_origin[0]; outOrigin[1] = g_origin[1]; outOrigin[2] = g_origin[2];
        outFovy = g_fovy;
        return true;
    }
}
