#pragma once
#define WIN32_LEAN_AND_MEAN

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace modutils
{

void initialize();
void enable_hooks();
void deinitialize();

struct ScanArgs
{
    const std::string aob;
    void *address = nullptr;
    const ptrdiff_t offset = 0;
    const std::vector<std::pair<ptrdiff_t, ptrdiff_t>> relative_offsets = {};
};

void *scan(const ScanArgs &args);

// Count how many times an AOB matches in the module image. Used by the signature
// health check to flag NON-UNIQUE signatures (>1 → scan() may resolve the wrong
// function, a latent bug). 0 = gone, 1 = unique, >1 = ambiguous.
size_t scan_count(const std::string &aob);

// ── Read a struct FIELD OFFSET live from the game's own code ──────────────────────────────
// The game compiles param/struct field offsets into its access instructions as the ModRM
// displacement, e.g. `movzx eax, byte ptr [row+0x3e]` carries 0x3e. Instead of hardcoding the
// constant (silently wrong after a patch that shifts the field), AOB the read site with the
// displacement bytes WILDCARDED, then read the live displacement out of the match — the offset
// becomes self-correcting: as long as the AOB still matches, the value is authoritative; if a
// patch moves the code the AOB FAILS loudly (resolve returns nullopt) instead of reading wrong.
//
// This is the displacement sibling of `relative_offsets` (which reads a rip-disp and uses it as a
// pointer delta); here we just RETURN the displacement as the field offset. `disp_pos` = byte
// position of the displacement within the AOB match; `disp_size` = 1 (disp8) or 4 (disp32).
// Refuses to resolve unless the AOB is UNIQUE (a wrong-site offset would be catastrophic).
//   Prototype + authoring tool: D:\ghidra_scripts\offset_resolver.py
struct FieldOffsetArgs
{
    const std::string aob;
    ptrdiff_t disp_pos = 0;
    int disp_size = 1; // 1 = disp8, 4 = disp32
    // When the field is read by a GENERIC/shared getter (e.g. one switch dispatcher reused across
    // several param types that share a text-field block), the AOB legitimately matches N>1 sites.
    // With consensus=true, resolve accepts N matches IFF every one reads the SAME displacement
    // (and returns it); a single disagreeing site → nullopt. Default false = require a unique match.
    bool consensus = false;
};

std::optional<ptrdiff_t> resolve_field_offset(const FieldOffsetArgs &args);

void hook(void *function, void *detour, void **trampoline);

template <typename ReturnType> inline ReturnType *scan(const ScanArgs &args)
{
    return reinterpret_cast<ReturnType *>(scan(args));
}

template <typename FunctionType>
inline FunctionType *hook(const ScanArgs &args, FunctionType &detour, FunctionType *&trampoline)
{
    auto function = scan<FunctionType>(args);
    if (function == nullptr)
    {
        throw std::runtime_error("Failed to find original function address");
    }
    hook(reinterpret_cast<void *>(function), reinterpret_cast<void *>(&detour),
         reinterpret_cast<void **>(&trampoline));
    return function;
}

};