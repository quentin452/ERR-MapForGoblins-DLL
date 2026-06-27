// Standalone parser coverage tool.
//
// Compiles and runs locally:
//     g++ -std=c++17 -DPARSER_COVERAGE -I../src -I../third_party test_msbe_coverage.cpp ../src/worldmap/msbe_parser.cpp ../src/stb_image_impl.cpp -o test_msbe_coverage
//
// Usage:
//     ./test_msbe_coverage <path_to_file.msb.dcx or path_to_file.emevd.dcx>
//
// Prints unread byte ranges in RED to help identify unparsed data fields or sections.

#include "worldmap/msbe_parser.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace goblin::msbe;

// ANSI escape codes for coloring
const char* RED = "\033[1;31m";
const char* RESET = "\033[0m";

void print_hex_dump(const uint8_t* buf, size_t start, size_t end)
{
    for (size_t i = start; i < end; i += 16)
    {
        std::printf("      %08zx: ", i);
        size_t limit = std::min(i + 16, end);
        for (size_t j = i; j < limit; ++j)
        {
            std::printf("%s%02x%s ", RED, buf[j], RESET);
        }
        // Align ASCII representation
        if (limit < i + 16)
        {
            for (size_t j = limit; j < i + 16; ++j) std::printf("   ");
        }
        std::printf(" | ");
        for (size_t j = i; j < limit; ++j)
        {
            char c = (buf[j] >= 32 && buf[j] < 127) ? (char)buf[j] : '.';
            std::printf("%s%c%s", RED, c, RESET);
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "Usage: %s <file.msb.dcx / file.emevd.dcx>\n", argv[0]);
        return 1;
    }

    std::string filepath = argv[1];
    std::ifstream file(filepath, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "Error: Could not open file: %s\n", filepath.c_str());
        return 1;
    }

    std::vector<uint8_t> file_data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::printf("Loaded %s (%zu bytes)\n", filepath.c_str(), file_data.size());

    // Decompress if DCX
    std::vector<uint8_t> parsed_data;
    bool isKrak = false;
    if (file_data.size() >= 4 && file_data[0] == 'D' && file_data[1] == 'C' && file_data[2] == 'X' && file_data[3] == '\0')
    {
        std::printf("Decompressing DCX container...\n");
        parsed_data = dcx_decompress(file_data.data(), file_data.size(), &isKrak, nullptr);
        if (parsed_data.empty())
        {
            if (isKrak)
            {
                std::fprintf(stderr, "Error: File is Oodle-compressed (KRAK), but Oodle is not available in this standalone tool.\n");
            }
            else
            {
                std::fprintf(stderr, "Error: DCX decompression failed.\n");
            }
            return 1;
        }
        std::printf("Decompressed to %zu bytes\n", parsed_data.size());
    }
    else
    {
        parsed_data = std::move(file_data);
    }

    // Determine type (MSB or EMEVD)
    bool is_msb = false;
    bool is_emevd = false;

    if (filepath.find(".msb") != std::string::npos)
    {
        is_msb = true;
    }
    else if (filepath.find(".emevd") != std::string::npos)
    {
        is_emevd = true;
    }
    else
    {
        // Try to guess from magic bytes
        if (parsed_data.size() >= 4 && parsed_data[0] == 'M' && parsed_data[1] == 'S' && parsed_data[2] == 'B' && parsed_data[3] == ' ')
        {
            is_msb = true;
        }
        else if (parsed_data.size() >= 4 && parsed_data[0] == 'E' && parsed_data[1] == 'V' && parsed_data[2] == 'D' && parsed_data[3] == '\0')
        {
            is_emevd = true;
        }
    }

    if (!is_msb && !is_emevd)
    {
        std::fprintf(stderr, "Error: Could not determine file type (MSB or EMEVD) for %s\n", filepath.c_str());
        return 1;
    }

    // Start coverage tracking
    start_coverage(parsed_data.data(), parsed_data.size());

    if (is_msb)
    {
        std::printf("Parsing as MSB...\n");
        ParseResult r = parse_msb(parsed_data.data(), parsed_data.size(), /*resident=*/false, 0,
                                  /*wantAssets=*/true, /*wantEnemies=*/true, /*wantRegions=*/true);
        std::printf("Parse result: %s (Treasures: %zu, Assets: %zu, Enemies: %zu, Regions: %zu)\n",
                    r.ok ? "OK" : "FAILED", r.treasures.size(), r.assets.size(), r.enemies.size(), r.regions.size());
    }
    else
    {
        std::printf("Parsing as EMEVD...\n");
        EmevdParse r = parse_emevd_full(parsed_data.data(), parsed_data.size());
        std::printf("Parse result: (Direct: %zu, Event1200: %zu, Setters: %zu, BossFlags: %zu, PerTile: %zu)\n",
                    r.direct.size(), r.runEvent1200.size(), r.setters.size(), r.bossFlagLot.size(), r.perTileEnemyAward.size());
    }

    // Get coverage map
    std::vector<uint8_t> coverage = get_coverage();
    stop_coverage();

    if (coverage.size() != parsed_data.size())
    {
        std::fprintf(stderr, "Error: Coverage map size mismatch.\n");
        return 1;
    }

    // Analyze unread byte ranges
    size_t unread_count = 0;
    size_t range_start = 0;
    bool in_unread_range = false;

    std::printf("\n== UNREAD BYTE RANGES (HIGHLIGHTED IN %sRED%s) ==\n", RED, RESET);

    for (size_t i = 0; i < coverage.size(); ++i)
    {
        if (coverage[i] == 0)
        {
            unread_count++;
            if (!in_unread_range)
            {
                range_start = i;
                in_unread_range = true;
            }
        }
        else
        {
            if (in_unread_range)
            {
                size_t range_len = i - range_start;
                std::printf("  Unread range: [0x%zx - 0x%zx] (%zu bytes)\n", range_start, i - 1, range_len);
                if (range_len <= 128)
                {
                    print_hex_dump(parsed_data.data(), range_start, i);
                }
                else
                {
                    print_hex_dump(parsed_data.data(), range_start, range_start + 48);
                    std::printf("      ... (%zu bytes truncated) ...\n", range_len - 96);
                    print_hex_dump(parsed_data.data(), i - 48, i);
                }
                in_unread_range = false;
            }
        }
    }

    if (in_unread_range)
    {
        size_t range_len = coverage.size() - range_start;
        std::printf("  Unread range: [0x%zx - 0x%zx] (%zu bytes)\n", range_start, coverage.size() - 1, range_len);
        print_hex_dump(parsed_data.data(), range_start, coverage.size());
    }

    double pct = (double)(parsed_data.size() - unread_count) / parsed_data.size() * 100.0;
    std::printf("\nCoverage Summary:\n");
    std::printf("  Total bytes:  %zu\n", parsed_data.size());
    std::printf("  Read bytes:   %zu\n", parsed_data.size() - unread_count);
    std::printf("  Unread bytes: %zu\n", unread_count);
    std::printf("  Coverage:     %.2f%%\n", pct);

    return 0;
}
