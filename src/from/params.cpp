#include <chrono>
#include <spdlog/spdlog.h>
#include <thread>

#include "../modutils.hpp"
#include "../re_signatures.hpp"
#include "params.hpp"

using namespace std;

from::params::ParamList **from::params::param_list_address = nullptr;

void from::params::initialize()
{
    param_list_address = modutils::scan<ParamList *>({
        .aob = goblin::sig::SOLO_PARAM_LIST,
        .relative_offsets = {{3, 7}},
    });

    spdlog::info("Waiting for params...");

    while (true)
    {
        auto param_list = *param_list_address;
        if (param_list != nullptr)
        {
            bool params_ready = true;
            for (int i = 0; i < sizeof(param_list->entries) / sizeof(param_list->entries[0]); i++)
            {
                auto param_res_cap = param_list->entries[i].param_res_cap;
                if (param_res_cap == nullptr)
                {
                    params_ready = false;
                    break;
                }
            }
            if (params_ready)
            {
                return;
            }
        }

        this_thread::sleep_for(chrono::seconds(1));
    }
}
