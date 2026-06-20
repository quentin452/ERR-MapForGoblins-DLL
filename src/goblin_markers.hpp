#pragma once
#include <cstdint>
#include <filesystem>

#include "goblin_map_data.hpp"

namespace goblin::markers
{
    // Human-readable name for a marker category (e.g. "Loot - Smithing Stones (Low)").
    const char *category_name(generated::Category c);

    // Write an event flag in the live game via EventFlagMan (same singleton the
    // reader uses). Returns false if the flag API can't be resolved. The SetEventFlag
    // function (Hexinton "EventFlag_C1") is resolved via the stable prologue AOB
    // (full Hexinton signature's tail drifts per game version). Foundation for
    // unlock-all-style writes.
    bool set_event_flag(uint32_t flag_id, uint8_t value);

    // Configure output file path (called once at DLL init).
    void set_output_path(std::filesystem::path path);

    // Poll hotkey and trigger dump. Runs forever in a worker thread.
    void hotkey_loop();
}
