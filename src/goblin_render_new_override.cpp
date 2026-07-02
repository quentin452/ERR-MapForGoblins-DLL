// Compiled ONLY into goblin_overlay_render.dll (see CMakeLists.txt's GOBLIN_OVERLAY_HOTRELOAD
// block), never into the host or the default single-DLL build.
//
// Why: /MT gives each DLL its own CRT heap. ~8 goblin::overlay_api functions pass std::string/
// std::vector across the host↔render boundary by value, by move, or via out-param
// (lookup_text_utf8, mask_to_combo_string, harvested_ids, grace_candidates, tpf_dds_at,
// register_runtime_entries, cfg_questProgress_ref/cfg_regionToggles_ref reallocation) — each of
// those would allocate on one CRT's heap and free on the other's (silent corruption). Rather than
// marshal every signature to POD, route ALL of this DLL's global operator new/delete to the HOST's
// heap via the MFG_HostAlloc/MFG_HostFree exports (goblin_overlay_render_loader.cpp): one heap for
// every C++ allocation on both sides, the entire api surface safe by construction — including
// after a hot reload frees this module (its allocations live on the host heap).
//
// ImGui is NOT covered here (it allocates via malloc wrappers, not operator new) — the host's
// allocator functions travel in OverlayFrameCtx and the extern "C" trampolines apply them with
// ImGui::SetAllocatorFunctions (goblin_overlay_render.cpp).

#if defined(GOBLIN_OVERLAY_HOTRELOAD_BUILD)

#include <cstddef>
#include <new>

extern "C" __declspec(dllimport) void *MFG_HostAlloc(size_t n);
extern "C" __declspec(dllimport) void MFG_HostFree(void *p);
extern "C" __declspec(dllimport) void *MFG_HostAllocAligned(size_t n, size_t a);
extern "C" __declspec(dllimport) void MFG_HostFreeAligned(void *p);

namespace
{
    void *host_new(std::size_t n)
    {
        for (;;)
        {
            if (void *p = MFG_HostAlloc(n ? n : 1)) return p;
            if (std::new_handler h = std::get_new_handler())
                h();
            else
                throw std::bad_alloc();
        }
    }
    void *host_new_aligned(std::size_t n, std::size_t a)
    {
        for (;;)
        {
            if (void *p = MFG_HostAllocAligned(n ? n : 1, a)) return p;
            if (std::new_handler h = std::get_new_handler())
                h();
            else
                throw std::bad_alloc();
        }
    }
}

void *operator new(std::size_t n) { return host_new(n); }
void *operator new[](std::size_t n) { return host_new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return MFG_HostAlloc(n ? n : 1); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return MFG_HostAlloc(n ? n : 1); }
void operator delete(void *p) noexcept { MFG_HostFree(p); }
void operator delete[](void *p) noexcept { MFG_HostFree(p); }
void operator delete(void *p, std::size_t) noexcept { MFG_HostFree(p); }
void operator delete[](void *p, std::size_t) noexcept { MFG_HostFree(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { MFG_HostFree(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { MFG_HostFree(p); }

void *operator new(std::size_t n, std::align_val_t a) { return host_new_aligned(n, static_cast<std::size_t>(a)); }
void *operator new[](std::size_t n, std::align_val_t a) { return host_new_aligned(n, static_cast<std::size_t>(a)); }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    return MFG_HostAllocAligned(n ? n : 1, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept
{
    return MFG_HostAllocAligned(n ? n : 1, static_cast<std::size_t>(a));
}
void operator delete(void *p, std::align_val_t) noexcept { MFG_HostFreeAligned(p); }
void operator delete[](void *p, std::align_val_t) noexcept { MFG_HostFreeAligned(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { MFG_HostFreeAligned(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { MFG_HostFreeAligned(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { MFG_HostFreeAligned(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { MFG_HostFreeAligned(p); }

#endif  // GOBLIN_OVERLAY_HOTRELOAD_BUILD
