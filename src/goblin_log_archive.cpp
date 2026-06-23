#include "goblin_log_archive.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // ── CRC32 (IEEE, zip standard) ──
    uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            init = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < len; ++i)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    // Little-endian appenders.
    void put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
    void put32(std::vector<uint8_t> &b, uint32_t v)
    {
        for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF);
    }

    struct Member { std::string name; std::vector<uint8_t> data; uint32_t crc; uint32_t offset; };

    // Write a store-only (method 0, uncompressed) zip of `members` to `out`.
    bool write_store_zip(const fs::path &out, std::vector<Member> &members)
    {
        std::vector<uint8_t> buf;
        // Local file headers + data.
        for (auto &m : members)
        {
            m.offset = (uint32_t)buf.size();
            put32(buf, 0x04034b50);                 // local file header sig
            put16(buf, 20);                          // version needed
            put16(buf, 0);                           // flags
            put16(buf, 0);                           // method 0 = store
            put16(buf, 0); put16(buf, 0);            // mod time / date (0 = unset)
            put32(buf, m.crc);                       // crc32
            put32(buf, (uint32_t)m.data.size());     // compressed size
            put32(buf, (uint32_t)m.data.size());     // uncompressed size
            put16(buf, (uint16_t)m.name.size());     // filename length
            put16(buf, 0);                           // extra length
            buf.insert(buf.end(), m.name.begin(), m.name.end());
            buf.insert(buf.end(), m.data.begin(), m.data.end());
        }
        // Central directory.
        uint32_t cd_start = (uint32_t)buf.size();
        for (auto &m : members)
        {
            put32(buf, 0x02014b50);                 // central dir header sig
            put16(buf, 20); put16(buf, 20);          // version made by / needed
            put16(buf, 0); put16(buf, 0);            // flags / method
            put16(buf, 0); put16(buf, 0);            // time / date
            put32(buf, m.crc);
            put32(buf, (uint32_t)m.data.size());
            put32(buf, (uint32_t)m.data.size());
            put16(buf, (uint16_t)m.name.size());
            put16(buf, 0); put16(buf, 0);            // extra / comment length
            put16(buf, 0); put16(buf, 0);            // disk start / internal attrs
            put32(buf, 0);                           // external attrs
            put32(buf, m.offset);                    // local header offset
            buf.insert(buf.end(), m.name.begin(), m.name.end());
        }
        uint32_t cd_size = (uint32_t)buf.size() - cd_start;
        // End of central directory.
        put32(buf, 0x06054b50);
        put16(buf, 0); put16(buf, 0);                // disk numbers
        put16(buf, (uint16_t)members.size());        // entries on disk
        put16(buf, (uint16_t)members.size());        // total entries
        put32(buf, cd_size);
        put32(buf, cd_start);
        put16(buf, 0);                               // comment length

        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char *>(buf.data()), (std::streamsize)buf.size());
        return f.good();
    }

    std::string timestamp_now()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char s[32];
        std::strftime(s, sizeof(s), "%Y%m%d_%H%M%S", &tm);
        return s;
    }

    bool is_session_log(const fs::path &p)
    {
        if (!p.has_filename()) return false;
        std::string n = p.filename().string();
        if (n.rfind("MapForGoblins", 0) != 0) return false; // must start with MapForGoblins
        std::string ext = p.extension().string();
        return ext == ".log" || n == "MapForGoblins_flagcapture.txt";
    }
}

void goblin::log_archive::archive_and_rotate(const fs::path &logs_dir, int keep_n)
{
    std::error_code ec;
    if (!fs::exists(logs_dir, ec)) return;

    // 1) Collect the previous session's log files (non-empty) from logs_dir root.
    std::vector<Member> members;
    std::vector<fs::path> to_delete;
    for (auto &de : fs::directory_iterator(logs_dir, ec))
    {
        if (ec) break;
        if (!de.is_regular_file(ec) || !is_session_log(de.path())) continue;
        std::ifstream in(de.path(), std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        to_delete.push_back(de.path()); // delete even if empty (clears stale 0-byte files)
        if (data.empty()) continue;
        Member m;
        m.name = de.path().filename().string();
        m.crc = crc32_update(0, data.data(), data.size());
        m.data = std::move(data);
        members.push_back(std::move(m));
    }

    // 2) Zip them into archive/<timestamp>.zip (only if there's content to keep).
    if (!members.empty())
    {
        fs::path archive_dir = logs_dir / "archive";
        fs::create_directories(archive_dir, ec);
        std::string base = "MapForGoblins_" + timestamp_now();
        fs::path zip = archive_dir / (base + ".zip");
        for (int i = 1; fs::exists(zip, ec); ++i) // avoid same-second collisions
            zip = archive_dir / (base + "_" + std::to_string(i) + ".zip");
        if (!write_store_zip(zip, members))
            return; // keep the .log files if the archive failed — don't lose data
    }

    // 3) Delete the archived originals so this session starts clean.
    for (auto &p : to_delete)
        fs::remove(p, ec);

    // 4) Retention: keep only the newest keep_n zips in archive/.
    fs::path archive_dir = logs_dir / "archive";
    if (keep_n > 0 && fs::exists(archive_dir, ec))
    {
        std::vector<fs::path> zips;
        for (auto &de : fs::directory_iterator(archive_dir, ec))
            if (!ec && de.is_regular_file(ec) && de.path().extension() == ".zip")
                zips.push_back(de.path());
        if ((int)zips.size() > keep_n)
        {
            std::sort(zips.begin(), zips.end(),
                      [&](const fs::path &a, const fs::path &b) {
                          std::error_code e1, e2;
                          return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
                      });
            for (size_t i = 0; i + (size_t)keep_n < zips.size(); ++i)
                fs::remove(zips[i], ec);
        }
    }
}
