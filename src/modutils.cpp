#include <codecvt>
#include <filesystem>
#include <locale>
#include <span>
#include <stdexcept>
#include <string>

#include <MinHook.h>
#include <Pattern16.h>
#include <spdlog/spdlog.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>

#include "modutils.hpp"

using namespace std;

static span<unsigned char> memory;

static string sus_filenames[] = {
    "ALI213.ini",      "ColdAPI.ini",   "ColdClientLoader.ini",  "CPY.ini",
    "ds.ini",          "hlm.ini",       "local_save.txt",        "SmartSteamEmu.ini",
    "steam_api.ini",   "steam_emu.ini", "steam_interfaces.ini",  "steam_settings",
    "SteamConfig.ini", "valve.ini",     "Language Selector.exe",
};

void modutils::initialize()
{
    HMODULE module_handle = GetModuleHandleA("eldenring.exe");
    if (!module_handle)
    {
        throw runtime_error("Failed to get handle for eldenring.exe process");
    }

    wstring_convert<codecvt_utf8_utf16<wchar_t>, wchar_t> convert;

    wchar_t exe_filename[MAX_PATH] = {0};
    GetModuleFileNameW(module_handle, exe_filename, MAX_PATH);
    spdlog::info("Found handle for eldenring.exe process: {}", convert.to_bytes(exe_filename));

    auto exe_directory = filesystem::path(exe_filename).parent_path();
    for (auto i = 0; i < size(sus_filenames); i++)
    {
        if (filesystem::exists(exe_directory / sus_filenames[i]))
        {
            spdlog::error("Game may be modified, compatibility is unlikely [{}]", i);
        }
    }

    MEMORY_BASIC_INFORMATION memory_info;
    if (VirtualQuery((void *)module_handle, &memory_info, sizeof(memory_info)) == 0)
    {
        throw runtime_error("Failed to get virtual memory information");
    }

    IMAGE_DOS_HEADER *dos_header = (IMAGE_DOS_HEADER *)module_handle;
    IMAGE_NT_HEADERS *nt_headers =
        (IMAGE_NT_HEADERS *)((ULONG64)memory_info.AllocationBase + (ULONG64)dos_header->e_lfanew);

    if ((dos_header->e_magic == IMAGE_DOS_SIGNATURE) &&
        (nt_headers->Signature == IMAGE_NT_SIGNATURE))
    {
        memory = {(unsigned char *)memory_info.AllocationBase,
                  nt_headers->OptionalHeader.SizeOfImage};
    }

    auto mh_status = MH_Initialize();
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error initializing MinHook: ") + MH_StatusToString(mh_status));
    }
}

void modutils::deinitialize()
{
    MH_Uninitialize();
}

void *modutils::scan(const ScanArgs &args)
{
    unsigned char *match;
    if (args.address != nullptr)
    {
        match = reinterpret_cast<unsigned char *>(args.address);
    }
    else if (!args.aob.empty())
    {
        match = reinterpret_cast<unsigned char *>(
            Pattern16::scan(&memory.front(), memory.size(), args.aob));
    }
    else
    {
        match = &memory.front();
    }

    if (match != nullptr)
    {
        match += args.offset;

        for (auto [first, second] : args.relative_offsets)
        {
            ptrdiff_t offset = *reinterpret_cast<const int *>(&match[first]) + second;
            match += offset;
        }

        return match;
    }

    return nullptr;
}

size_t modutils::scan_count(const std::string &aob)
{
    if (memory.empty() || aob.empty())
        return 0;
    size_t count = 0;
    unsigned char *start = &memory.front();
    size_t remaining = memory.size();
    while (remaining)
    {
        auto *m = reinterpret_cast<unsigned char *>(Pattern16::scan(start, remaining, aob));
        if (!m)
            break;
        ++count;
        size_t consumed = static_cast<size_t>(m - start) + 1;
        if (consumed >= remaining)
            break;
        start = m + 1;
        remaining -= consumed;
    }
    return count;
}

std::optional<ptrdiff_t> modutils::resolve_field_offset(const FieldOffsetArgs &args)
{
    // A field offset resolved from the WRONG instruction is silently catastrophic, so demand the
    // AOB be unique before trusting it (mirrors the [SIG] uniqueness check). 0 = patch broke it.
    size_t cnt = scan_count(args.aob);
    if (cnt != 1)
    {
        spdlog::warn("[FIELDOFF] AOB matched {} sites (need exactly 1) — refusing to resolve [{}]",
                     cnt, args.aob);
        return std::nullopt;
    }

    auto *match = reinterpret_cast<const unsigned char *>(scan({.aob = args.aob}));
    if (match == nullptr)
        return std::nullopt;

    const unsigned char *d = match + args.disp_pos;
    ptrdiff_t off = (args.disp_size == 1) ? static_cast<ptrdiff_t>(*reinterpret_cast<const int8_t *>(d))
                                          : static_cast<ptrdiff_t>(*reinterpret_cast<const int32_t *>(d));
    spdlog::info("[FIELDOFF] resolved offset 0x{:x} live from access site {}",
                 off, static_cast<const void *>(match));
    return off;
}

void modutils::hook(void *function, void *detour, void **trampoline)
{
    auto mh_status = MH_CreateHook(function, detour, trampoline);
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error creating hook: ") + MH_StatusToString(mh_status));
    }
    mh_status = MH_QueueEnableHook(function);
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error queueing hook: ") + MH_StatusToString(mh_status));
    }
}

void modutils::enable_hooks()
{
    auto mh_status = MH_ApplyQueued();
    if (mh_status != MH_OK)
    {
        throw runtime_error(string("Error enabling hooks: ") + MH_StatusToString(mh_status));
    }
}
