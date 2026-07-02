// mfg_inigen — emits a default MapForGoblins.ini from the in-code schema.
// Build-time tool so the shipped ini is generated, never hand-maintained.
//
//   mfg_inigen <out.ini>
//
// Single-DLL migration: the ini always includes the ERR-only sections/entries;
// the DLL force-disables them at load time on a non-ERR install (runtime
// detection, see goblin::err_features_enabled). The old --vanilla/--err flags
// are accepted and ignored for build-script compatibility.

#include "goblin_config_schema.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
    std::string out_path;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (!a.empty() && a[0] != '-')
            out_path = a;
    }

    if (out_path.empty())
    {
        std::cerr << "usage: mfg_inigen <out.ini>\n";
        return 2;
    }

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        std::cerr << "mfg_inigen: cannot write " << out_path << "\n";
        return 1;
    }

    goblin::emit_ini(f, /*include_err_only=*/true);
    std::cout << "mfg_inigen: wrote " << out_path << "\n";
    return 0;
}
