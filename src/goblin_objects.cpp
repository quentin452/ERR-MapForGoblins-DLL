#include "goblin_objects.hpp"

#include "goblin_add_collision.hpp"   // add_collision::add_box (walkable Havok box)
#include "goblin_inject.hpp"          // get_player_world_pos — in-world gate
#include "goblin_r3d.hpp"             // r3d::add_box_ex / set_enabled — mod-owned 3D render

#include <spdlog/spdlog.h>

#define TOML_EXCEPTIONS 0  // parse errors returned, not thrown — the ONLY config that parses under Proton
#include <toml++/toml.hpp>

#include <mutex>

namespace goblin::objects
{
namespace
{
std::vector<ObjectDef> g_defs;
std::filesystem::path g_folder;
std::mutex g_mtx;
bool g_try_collision = false;   // experimental walkable-collision pass (frame + borrowed-shape caveats)

// Read a 3-float array (pos/rot/size/half) from a toml node into out[3]; returns true if present+sized.
bool read_vec3(const toml::node_view<const toml::node> &n, float out[3])
{
    if (!n.is_array()) return false;
    const toml::array *a = n.as_array();
    if (!a || a->size() < 3) return false;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<float>(a->at(i).value_or(0.0));
    return true;
}
}  // namespace

int load(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        spdlog::info("[OBJECTS] no objects file at {}", path.string());
        return 0;
    }
    auto result = toml::parse_file(path.string());
    if (!result)
    {
        spdlog::warn("[OBJECTS] parse failed {}: {}", path.string(),
                     std::string(result.error().description()));
        return -1;
    }
    const toml::table &root = result.table();
    const toml::array *arr = root["object"].as_array();
    std::vector<ObjectDef> parsed;
    if (arr)
    {
        for (const toml::node &node : *arr)
        {
            const toml::table *t = node.as_table();
            if (!t) continue;
            ObjectDef o;
            o.id = (*t)["id"].value_or("");
            o.world = (*t)["world"].value_or("");
            o.kind = (*t)["kind"].value_or(std::string("platform"));
            o.relative = (*t)["relative"].value_or(true);
            read_vec3((*t)["pos"], o.pos);
            read_vec3((*t)["rot"], o.rot);
            // collision: prefer HALF via `half`, else full `size`/2. shape optional (default box).
            if (const toml::table *c = (*t)["collision"].as_table())
            {
                o.has_collision = true;
                o.coll_shape = (*c)["shape"].value_or(std::string("box"));
                float tmp[3];
                if (read_vec3((*c)["half"], tmp)) { for (int i = 0; i < 3; ++i) o.coll_half[i] = tmp[i]; }
                else if (read_vec3((*c)["size"], tmp)) { for (int i = 0; i < 3; ++i) o.coll_half[i] = tmp[i] * 0.5f; }
            }
            // render block
            if (const toml::table *r = (*t)["render"].as_table())
            {
                o.has_render = true;
                o.backend = (*r)["backend"].value_or(std::string("mesh3d"));
                o.primitive = (*r)["primitive"].value_or(std::string("box"));
                float tmp[3];
                if (read_vec3((*r)["half"], tmp)) { for (int i = 0; i < 3; ++i) o.render_half[i] = tmp[i]; }
                else if (read_vec3((*r)["size"], tmp)) { for (int i = 0; i < 3; ++i) o.render_half[i] = tmp[i] * 0.5f; }
                else { for (int i = 0; i < 3; ++i) o.render_half[i] = o.coll_half[i]; }  // fall back to collision
            }
            // If neither block given, a bare object with pos → default a small platform (collision+render).
            if (!o.has_collision && !o.has_render) { o.has_collision = true; o.has_render = true; }
            parsed.push_back(std::move(o));
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_defs = std::move(parsed);
    }
    spdlog::info("[OBJECTS] loaded {} object(s) from {}", g_defs.size(), path.string());
    return static_cast<int>(g_defs.size());
}

int load_boot(const std::filesystem::path &mod_folder)
{
    g_folder = mod_folder;
    return load(mod_folder / "objects.toml");
}

int realize()
{
    float px, py, pz;
    if (!goblin::get_player_world_pos(px, py, pz))
    {
        spdlog::warn("[OBJECTS] realize skipped — not in-world");
        return 0;
    }
    std::vector<ObjectDef> defs;
    bool try_coll;
    { std::lock_guard<std::mutex> lk(g_mtx); defs = g_defs; try_coll = g_try_collision; }

    int made = 0;
    for (const ObjectDef &o : defs)
    {
        // World-frame render position: pos is an offset from the player (relative) or absolute.
        float wpos[3] = {o.pos[0], o.pos[1], o.pos[2]};
        if (o.relative) { wpos[0] += px; wpos[1] += py; wpos[2] += pz; }

        // Gap log: fields/primitives slice 1 can't honor yet (the checklist writes itself).
        if (o.rot[0] != 0.f || o.rot[1] != 0.f || o.rot[2] != 0.f)
            spdlog::info("[OBJECTS] gap: '{}' rot=({:.1f},{:.1f},{:.1f}) not honored (box is axis-aligned)",
                         o.id, o.rot[0], o.rot[1], o.rot[2]);
        if (o.has_render && o.primitive != "box")
            spdlog::info("[OBJECTS] gap: '{}' render primitive '{}' not honored (only box)", o.id, o.primitive);

        // Slice 1 deliverable: the mod-owned 3D greybox render, driven from the TOML.
        if (o.has_render && o.primitive == "box")
        {
            goblin::r3d::add_box_ex(wpos, o.render_half);
            ++made;
        }

        // Experimental walkable collision (off by default). Borrowed shape + havok frame ≠ r3d frame,
        // so it may be mis-sized/mis-placed — a slice-2 job (box-builder + frame conversion).
        if (try_coll && o.has_collision && o.coll_shape == "box")
        {
            auto res = goblin::add_collision::add_box(o.coll_half, wpos, /*force=*/true);
            spdlog::info("[OBJECTS] '{}' EXPERIMENTAL collision add_box -> {} (⚠ borrowed shape + havok "
                         "frame; alignment is slice-2)", o.id, res.ok ? "ok" : "FAILED");
        }

        spdlog::info("[OBJECTS] realized '{}' kind={} wpos=({:.1f},{:.1f},{:.1f}) half=({:.2f},{:.2f},{:.2f}) "
                     "render={}({}) rel={}", o.id, o.kind, wpos[0], wpos[1], wpos[2],
                     o.render_half[0], o.render_half[1], o.render_half[2], o.has_render, o.backend, o.relative);
    }
    // ⚠ Do NOT auto-enable r3d: the D3D12 backend HANGS the GPU on native Windows (TDR → CPU-100%
    // freeze, seen 2026-07-07 — r3d was only ever live-verified on Linux/Proton via vkd3d, which is
    // more forgiving than native D3D12). The boxes are registered; enable the render explicitly with
    // `r3d 1` (at your own risk on Windows) once r3d is D3D12-hardened, or use an ImGui render backend.
    spdlog::info("[OBJECTS] realized {}/{} render object(s) (r3d NOT auto-enabled — Windows D3D12 hang; "
                 "collision {})", made, (int)defs.size(), try_coll ? "experimental ON" : "off");
    return made;
}

void set_try_collision(bool on) { std::lock_guard<std::mutex> lk(g_mtx); g_try_collision = on; }
bool try_collision() { std::lock_guard<std::mutex> lk(g_mtx); return g_try_collision; }

void clear_render()
{
    goblin::r3d::clear_boxes();
    spdlog::info("[OBJECTS] render cleared (Havok collision bodies persist until area reload — no remove path yet)");
}

const std::vector<ObjectDef> &defs() { return g_defs; }

std::string command(const std::string &rest)
{
    size_t b = rest.find_first_not_of(" \t");
    std::string sub = b == std::string::npos ? std::string{} : rest.substr(b);
    size_t e = sub.find_first_of(" \t");
    std::string arg = e == std::string::npos ? std::string{} : sub.substr(e + 1);
    if (e != std::string::npos) sub = sub.substr(0, e);

    if (sub == "reload")
    {
        int n = load(g_folder / "objects.toml");
        if (n < 0) return "err objects reload: parse failed (see [OBJECTS] log)";
        int m = realize();
        return "ok objects reloaded=" + std::to_string(n) + " realized=" + std::to_string(m);
    }
    if (sub == "load")
    {
        if (arg.empty()) return "err usage: objects load <path>";
        int n = load(arg);
        if (n < 0) return "err objects load: parse failed";
        int m = realize();
        return "ok objects loaded=" + std::to_string(n) + " realized=" + std::to_string(m);
    }
    if (sub == "realize")
    {
        int m = realize();
        return "ok objects realized=" + std::to_string(m) + " (r3d " +
               std::to_string(goblin::r3d::box_count()) + " boxes)";
    }
    if (sub == "clear")
    {
        clear_render();
        return "ok objects render cleared";
    }
    if (sub == "collision")
    {
        set_try_collision(arg != "0" && arg != "off");
        return std::string("ok objects collision (experimental) ") + (try_collision() ? "ON" : "off");
    }
    // list (default)
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string out = "ok objects loaded=" + std::to_string(g_defs.size()) + ":";
    for (const ObjectDef &o : g_defs)
    {
        char c[128];
        std::snprintf(c, sizeof(c), " [%s %s (%.0f,%.0f,%.0f) h(%.1f,%.1f,%.1f)]",
                      o.id.c_str(), o.kind.c_str(), o.pos[0], o.pos[1], o.pos[2],
                      o.coll_half[0], o.coll_half[1], o.coll_half[2]);
        out += c;
    }
    return out;
}
}  // namespace goblin::objects
