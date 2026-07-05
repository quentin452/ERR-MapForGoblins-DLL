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

    // Live-tunable projection interpretation (nailed via `w2s_probe`, then locked in code).
    int   g_conv = 0;        // 0 rowvec+Zf, 1 rowmajor+Zf, 2 rowvec-Zf, 3 rigid-inverse rowvec
    float g_fovy = 0.7505f;  // vertical FOV (radians) ~43 deg — refined from the probe
    bool  g_dot  = false;

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

    // Crash-safe windowed scan for the GameRendCameraSet vtable -> the camera instance whose +0xE0 is a
    // valid VIEW matrix. Cached; re-validated (vtable qword still present) each call.
    uintptr_t find_cam_instance()
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
        uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;
        uintptr_t aend = (uintptr_t)si.lpMaximumApplicationAddress;
        const size_t WIN = 8 * 1024 * 1024, OVL = 0x200;
        std::vector<uint8_t> buf(WIN + OVL);
        MEMORY_BASIC_INFORMATION mbi;
        while (addr < aend && VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            uintptr_t rbase = (uintptr_t)mbi.BaseAddress; size_t rsz = mbi.RegionSize; DWORD pr = mbi.Protect;
            bool ok = mbi.State == MEM_COMMIT && rsz >= 16 &&
                      (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                      !(pr & (PAGE_GUARD | PAGE_NOACCESS));
            if (ok)
                for (size_t off = 0; off < rsz; off += WIN)
                {
                    size_t want = rsz - off; if (want > WIN + OVL) want = WIN + OVL;
                    if (!safe_read((void *)(rbase + off), buf.data(), want)) continue;
                    size_t scan_to = (want > WIN) ? WIN : (want >= 8 ? want - 8 : 0);
                    for (size_t i = 0; i < scan_to; i += 8)
                        if (*(const uint64_t *)(buf.data() + i) == vt)
                        {
                            uintptr_t hit = rbase + off + i; float m[16];
                            if (rd(hit + VIEW_FROM_HIT, m) && looks_like_view(m)) { s_hit = hit; return hit; }
                        }
                }
            uintptr_t nxt = rbase + rsz; if (nxt <= addr) break; addr = nxt;
        }
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
        else  // 0 or 2: row-vector v*M
        {
            vx = m[0] * x + m[4] * y + m[8] * z + m[12];
            vy = m[1] * x + m[5] * y + m[9] * z + m[13];
            vz = m[2] * x + m[6] * y + m[10] * z + m[14];
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
}

namespace goblin::w2s
{
    void set_debug_dot(bool on) { g_dot = on; }

    std::string probe()
    {
        uintptr_t base = (uintptr_t)GetModuleHandleA("eldenring.exe");
        if (!base) return "err: eldenring.exe base not found";
        uintptr_t hit = find_cam_instance();
        if (!hit) return "err: no GameRendCameraSet instance with a valid VIEW matrix (in-world? camera active?)";

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
        // report every interpretation: view-space coords + resulting screen px
        for (int c = 0; c < 4; ++c)
        {
            float vx, vy, vz, fwd; to_view(m, px, py, pz, c, vx, vy, vz, fwd);
            float sx = -1, sy = -1; bool vis = project(m, px, py, pz, c, g_fovy, W, H, sx, sy);
            std::snprintf(b, sizeof(b), " conv%d: view=(%.2f,%.2f,%.2f) fwd=%.2f -> px=(%.0f,%.0f) %s\n",
                          c, vx, vy, vz, fwd, sx, sy, vis ? "" : "(behind)");
            out += b;
        }
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
}
