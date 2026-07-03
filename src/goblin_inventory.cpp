#include "goblin_inventory.hpp"

#include "goblin_debug_events.hpp"  // last_inventory_accessor — fallback inv
#include "modutils.hpp"
#include "re_signatures.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace goblin::inventory
{
namespace
{
// AddItemFunc(rcx=inv, rdx=&entry, r8=buffer, r9=count(0)). 4 args; extra regs ignored.
using AddItemFn = uint64_t(*)(void *inv, void *entry, void *buffer, uint64_t count);

AddItemFn g_add_item = nullptr;
void **g_inv_slot = nullptr;   // *g_inv_slot == the live MapItemMan (re-read each call)
std::atomic<bool> g_ready{false};

// The itembuffer replicated verbatim from the CT's ItemGib (10 qwords). Only qword[4] (the
// entry at +0x20) is patched per call: low dword = qty (i32), high dword = item id. The
// trailing 0xFFFFFFFF sentinels terminate the entry list — kept exactly as the CT sets them.
constexpr uint64_t kBufTemplate[10] = {
    0, 0, 0, 0,
    0xF00006AE00000001ull,  // [4] entry {qty=1, id=F00006AE} — patched per call
    0x0000000000000001ull,
    0xFFFFFFFFFFFFFFFFull,
    0xFFFFFFFF00000000ull,
    0xFFFFFFFFFFFFFFFFull,
    0xFFFFFFFF00000000ull,
};
}  // namespace

void initialize()
{
    if (g_ready.load()) return;
    // AddItemFunc — its own prologue AOB.
    g_add_item = reinterpret_cast<AddItemFn>(modutils::scan<void>({.aob = goblin::sig::ADD_ITEM_FUNC}));
    // MapItemMan slot from INVENTORY_ACCESSOR: the `mov rcx,[rip+disp32]` at match+0x16 →
    // slot = (match + 0x1D) + *(int32*)(match + 0x19).
    if (auto *m = reinterpret_cast<uint8_t *>(
            modutils::scan<void>({.aob = goblin::sig::INVENTORY_ACCESSOR})))
    {
        int32_t disp = *reinterpret_cast<int32_t *>(m + 0x19);
        g_inv_slot = reinterpret_cast<void **>(m + 0x1D + disp);
    }
    g_ready.store(true);
    spdlog::info("[INVGRANT] AddItemFunc={} inv_slot={} (accessor AOB {})",
                 (void *)g_add_item, (void *)g_inv_slot, g_inv_slot ? "OK" : "MISS");
}

void *accessor()
{
    // Live re-read of the static slot (the singleton can be created after init). Fall back to
    // the observer-captured pointer if the AOB didn't resolve.
    if (g_inv_slot)
    {
        // RPM (kernel call, not elidable, false on a bad ptr) — the slot may be unmapped
        // before the singleton exists.
        void *inv = nullptr;
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), g_inv_slot, &inv, sizeof(inv), &got) &&
            got == sizeof(inv) && reinterpret_cast<uintptr_t>(inv) >= 0x10000)
            return inv;
    }
    return goblin::debug_events::last_inventory_accessor();
}

// The raw game call, isolated so the caller's __try wraps a lone opaque CALL (clang-cl keeps
// that; a __try around inline loads gets elided — see clang-cl-seh-noinline).
__declspec(noinline) static uint64_t call_add_item(void *inv, void *entry, void *buffer)
{
    return g_add_item(inv, entry, buffer, 0);
}

static bool give_item_seh(void *inv, void *entry, void *buffer)
{
    __try
    {
        call_add_item(inv, entry, buffer);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool give_item(uint32_t item_id, int32_t qty)
{
    if (!g_ready.load()) initialize();
    if (!g_add_item) { spdlog::warn("[INVGRANT] AddItemFunc unresolved — give_item skipped"); return false; }
    void *inv = accessor();
    if (!inv) { spdlog::warn("[INVGRANT] no inventory accessor (no grant seen + AOB miss) — skipped"); return false; }

    uint64_t buf[10];
    std::memcpy(buf, kBufTemplate, sizeof(buf));
    // entry @ +0x20 = buf[4]: low dword = qty (as raw i32 bits), high dword = item id.
    buf[4] = (static_cast<uint64_t>(item_id) << 32) | static_cast<uint32_t>(qty);

    void *entry = reinterpret_cast<uint8_t *>(buf) + 0x20;  // rdx
    bool ok = give_item_seh(inv, entry, buf);               // r8 = buf base
    spdlog::info("[INVGRANT] give_item id={:#010x} qty={} inv={} -> {}", item_id, qty, inv,
                 ok ? "called" : "FAULTED");
    return ok;
}
}  // namespace goblin::inventory
