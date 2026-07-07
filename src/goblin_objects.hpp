#pragma once
#include <filesystem>
#include <string>
#include <vector>

// goblin_objects — the objects.toml realizer (virtual_world_3d_backend_plan.md, sequencing step 3,
// slice 1). Data-driven greybox worlds: a TOML `[[object]]` list is parsed into ObjectDef records,
// then REALIZED in-world into a walkable Havok box (goblin::add_collision) + a mod-owned 3D render
// (goblin::r3d). This connects the two existing primitives through a data file — author a walkable
// world from TOML, no FromSoft asset (no .flver/.hkx/.tpf/MSB) → mod-agnostic + light by construction.
//
// Slice 1 = box objects only (kind platform/wall/box). The realizer LOGS every field/primitive it can't
// yet honor ([OBJECTS] gap lines) so the missing-feature checklist writes itself as you author worlds.
// Later slices: more primitives (sphere/cylinder/ramp), CSG parts, generators, solid greybox material.
namespace goblin::objects
{
    struct ObjectDef
    {
        std::string id;
        std::string world;                 // virtual-world name (informational for slice 1)
        float pos[3] = {0.f, 0.f, 0.f};    // position; frame per `relative` (default = offset from player)
        bool relative = true;              // pos is an OFFSET from the player's current pos (the r3d frame),
                                           // so objects land near you regardless of the absolute frame;
                                           // set relative=false in TOML for an absolute r3d-world position
        float rot[3] = {0.f, 0.f, 0.f};    // euler deg — NOT honored yet (logged as a gap)
        std::string kind = "platform";     // platform|wall|ramp|beacon|trigger|spawn|dummy

        bool has_collision = false;
        std::string coll_shape = "box";    // only "box" honored in slice 1
        float coll_half[3] = {1.f, 0.25f, 1.f};   // HALF-extents (size/2)

        bool has_render = false;
        std::string backend = "mesh3d";    // imgui|mesh3d — both draw an r3d box in slice 1
        std::string primitive = "box";     // only "box" honored in slice 1
        float render_half[3] = {1.f, 0.25f, 1.f};
    };

    // Parse a `objects.toml` into the def list (REPLACES it). Returns #objects parsed (-1 on read/parse
    // error). Does NOT realize (add_collision needs the live hknpWorld = in-world only).
    int load(const std::filesystem::path &path);
    // Boot helper: load `<mod_folder>/objects.toml` if present (set the folder for later reload). #objects.
    int load_boot(const std::filesystem::path &mod_folder);

    // Realize every loaded def IN-WORLD: r3d greybox render (slice 1's solid deliverable) + — when
    // collision is enabled (experimental, off by default) — a best-effort walkable add_collision box.
    // Returns #objects rendered. MUST be called in-world — safe no-op otherwise (logs).
    // ⚠ collision is EXPERIMENTAL: add_collision borrows a live body's SHAPE (extents not yet honored)
    // and uses the havok physics frame (offset from the r3d frame by the block origin) — so a walkable
    // box may be mis-sized/mis-placed until the box-builder + frame conversion land (slice 2).
    int realize();
    void set_try_collision(bool on);   // arm the experimental walkable-collision pass (default OFF)
    bool try_collision();
    // Clear the rendered boxes (r3d). NB any Havok collision bodies are NOT removed (no remove path
    // yet) — they persist until the next area load.
    void clear_render();

    const std::vector<ObjectDef> &defs();

    // RPC `objects [list|reload|realize|clear|load <path>]` (default = list). See rpc-commands.md.
    std::string command(const std::string &rest);
}
