#pragma once
#include <filesystem>

#include "goblin_map_data.hpp"

namespace goblin::markers
{
    // Human-readable name for a marker category (e.g. "Loot - Smithing Stones (Low)").
    const char *category_name(generated::Category c);

    // Configure output file path (called once at DLL init).
    void set_output_path(std::filesystem::path path);

    // Poll hotkey and trigger dump. Runs forever in a worker thread.
    void hotkey_loop();
}
