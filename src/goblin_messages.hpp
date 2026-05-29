#pragma once

#include <cstdint>

namespace goblin
{
    /// Inject PlaceName text entries into MsgRepositoryImp at runtime.
    void setup_messages();

    /// Toggle the PlaceName FMG slot between vanilla and expanded states.
    void set_fmg_injection_active(bool active);
    bool is_fmg_injection_active();
}
