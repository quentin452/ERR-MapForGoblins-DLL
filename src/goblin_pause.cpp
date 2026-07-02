#include "goblin_pause.hpp"

#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mutex>

namespace goblin::pause
{
    namespace
    {
        // The two bytes of the frame-step branch: 0F 84 = je = RUNNING, 0F 85 = jne = PAUSED.
        uint8_t *g_branch = nullptr;

        uint8_t *resolve()
        {
            static std::once_flag once;
            std::call_once(once, [] {
                // Tolerant variant of sig::PAUSE_BRANCH: first two bytes wildcarded so the scan
                // still finds the branch if another pauser (PauseTheGame.dll) already flipped it
                // to 0F 85 before we looked. The health-check table keeps the strict 0F 84 form
                // (it runs at init, pre-flip).
                g_branch = modutils::scan<uint8_t>(
                    {.aob = "?? ?? ?? ?? ?? ?? C6 83 ?? ?? 00 00 00 48 8D ?? ?? ?? ?? ?? 48 89 ?? ?? 89"});
                if (g_branch && (g_branch[0] != 0x0F || (g_branch[1] != 0x84 && g_branch[1] != 0x85)))
                {
                    spdlog::warn("[PAUSE] branch candidate at {} is not je/jne ({:02x} {:02x}) — disabled",
                                 static_cast<void *>(g_branch), g_branch[0], g_branch[1]);
                    g_branch = nullptr;
                }
                spdlog::info("[PAUSE] frame-step branch {}", g_branch ? "resolved" : "NOT found — pause disabled");
            });
            return g_branch;
        }
    }

    bool available() { return resolve() != nullptr; }

    bool paused()
    {
        uint8_t *b = resolve();
        return b && b[1] == 0x85;
    }

    void set_paused(bool want)
    {
        uint8_t *b = resolve();
        if (!b) return;
        const uint8_t target = want ? 0x85 : 0x84;  // jne = paused, je = running
        if (b[1] == target) return;
        DWORD old = 0;
        if (!VirtualProtect(b, 2, PAGE_EXECUTE_READWRITE, &old))
        {
            spdlog::error("[PAUSE] VirtualProtect failed, gle={}", GetLastError());
            return;
        }
        b[1] = target;
        VirtualProtect(b, 2, old, &old);
        FlushInstructionCache(GetCurrentProcess(), b, 2);
        spdlog::info("[PAUSE] game {}", want ? "PAUSED" : "resumed");
    }
}
