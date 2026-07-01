#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace goblin
{
    // Create the ini from the schema defaults if missing; otherwise migrate it
    // in place (add new keys, apply renames, comment out obsolete keys),
    // preserving user-set values. Then load values into goblin::config.
    void load_config(const std::filesystem::path &ini_path);
    void ensure_ini(const std::filesystem::path &ini_path);

    // Write the current [Display Sections] section_* values back to the ini,
    // preserving the rest of the file. Called when the in-game section toggle
    // changes a group's visibility so the choice survives relaunch.
    void save_section_states(const std::filesystem::path &ini_path);

    // Persist EVERY bool config var (sections + the 63 categories + other
    // toggles) to the ini, preserving the rest of the file. The menu's Save
    // button calls this after syncing runtime visibility into the config vars.
    void save_all_bool_settings(const std::filesystem::path &ini_path);

    // Re-seed every config var from its schema default, then persist to the ini
    // (the menu's "Reset parameters to default" danger button). Does NOT touch
    // the live runtime visibility atomics — those are seeded at inject time, so
    // a game restart is needed to fully apply the defaults.
    void reset_to_defaults_and_save(const std::filesystem::path &ini_path);

    // The ini path last passed to load_config(), for code (e.g. the in-game
    // section toggle) that needs to persist back without threading the path.
    const std::filesystem::path &config_ini_path();

    namespace config
    {
        extern uint8_t loadDelay;
        extern bool requireMapFragments;
        extern bool bakedOnly;  // diag overlay: draw ONLY Baked-source markers (the no-bake residual)
        extern bool collectedGraying;
        extern bool hideCollected;
        extern bool stackIdenticalItems;  // merge co-located identical-item loot markers into one "xN"
        extern bool clusterDebugRadius;
        extern bool clusterDebugMarkers;   // per-marker projection/tile state dots (cluster diagnosis)
        extern bool showRegionLabels; // overlay map: draw major-region names (Limgrave, Caelid, ...)
        extern bool nativeItemIcons;  // overlay map: real game item icon (GPU harvest) when resident
        extern bool diagLootFlags;    // one-shot [LOOTDIAG]: dump all candidate pickup flags per loot lot
        extern bool diagLootPos;      // one-shot [LOOTPOS]: live MsbPart pos vs baked MAP_ENTRY placement
        extern bool diagMapOpens;     // [MAPOPEN]: hook CreateFileW, log map .msb.dcx opens (path+latency)
        extern bool diagFieldinsJoin; // one-shot [FIELDINS]: geom+0x3A8 embedded pool → child FieldIns lotId@+0x50
        extern bool diagLotMemscan;   // one-shot [LOTSCAN]: brute scan of committed private mem for a known lotId
        extern bool debugLogging;
        extern bool showAll;  // master switch: show every category (see
                              // is_category_enabled) except those listed below
        extern bool iconsHidden;  // persisted master off (menu 'Show icons' / F10)
        extern uint32_t overlayToggleKey;  // VK_* for the overlay menu toggle (default F1)
        extern uint16_t overlayToggleGamepad;  // XINPUT_GAMEPAD_* combo mask for the overlay toggle (default Y+R3)
        extern uint8_t virtualKeyboardLayout;  // 0 = Alphabetical, 1 = QWERTY (on-screen gamepad text entry)
        extern std::string showAllExcept;  // comma-separated category names to
                              // keep hidden even when showAll is on (matched
                              // loosely against the category display name)

        // One bool per goblin::generated::Category, indexed by the enum value.
        // Replaces the former ~65 individual show* category flags; access via
        // category_config_ptr()/is_category_enabled() in goblin_inject.cpp.
        extern bool showCategory[];

        // World - Bosses
        extern bool hideKilledBosses;  // true=hide killed icons, false=green checkmark

        // Compatibility
        extern bool liveLootLabels; // read live ItemLotParam item+category at runtime
                                    // → loot marker name shows the item the lot now
                                    // gives (Randomizer-safe). Needs full-band FMG copy.
                                    // (live_loot_flags/icons removed in Phase 2b — native-only.)
        extern bool anonymousLoot;  // spoiler-free mode: every loot marker shows a
                                    // gray "?" icon + a generic localized label instead
                                    // of the real item (blind randomizer runs).

        // ── Loot source (experimental) ──────────────────────────────────
        // When true, derive the TREASURE loot slice from the ACTIVE mod's REAL
        // map/MapStudio/*.msb.dcx files on disk instead of the committed bake:
        // disk placements REPLACE any baked lot whose itemLotId the MSBs place
        // (lotId-coverage), while EMEVD-granted + enemy-drop lots stay baked.
        // Reads loose DCX_DFLT (zlib) maps (ERR's modified ones); KRAK/Oodle-only
        // maps are skipped for now. Opt-in; see msbe_parser + the RE docs.
        // ALWAYS ON (no longer an INI toggle): the static bake is retired, so the disk
        // MSB + live passes are the ONLY marker source — turning them off would empty the
        // map. Kept as compile-time constants so every existing `config::loot*` call site
        // is unchanged. (Baked variants merge disk over their bake; dedup is provenance-
        // exclusive, no double-marking — see map_entry_layer finalize.)
        inline constexpr bool lootFromDiskMsb = true;
        // When true, also emit markers for the mod's AEG gather/collectible assets
        // (read from the same disk MSBs as lootFromDiskMsb). Item identity resolved
        // live via AssetEnvironmentGeometryParam.pickUpItemLotParamId → ItemLotParam.
        inline constexpr bool lootCollectibles = true;
        // When true, also emit markers for enemy-drop loot derived from the mod's
        // MSB Parts.Enemies (NPCParamID → NpcParam.itemLotId_map/_enemy → ItemLotParam,
        // all live). Replaces the matching baked LootSource::Enemy rows. Opt-in; see
        // memory msbe-enemy-loot-offsets + docs/re/windows_enemy_loot_nobake_analysis.md.
        inline constexpr bool lootEnemyDrops = true;
        // When true, also emit markers for EMEVD-scripted item awards: the mod's
        // event\*.emevd.dcx files are parsed, bank-2000 template-award inits give
        // (entityId, lotId), and the entityId is joined to its MSB Enemy part for the
        // position. Replaces the matching baked LootSource::Emevd rows. Opt-in; see
        // docs/re/windows_enemy_loot_nobake_analysis.md §5b + msbe::parse_emevd.
        inline constexpr bool lootEmevdDrops = true;
        // When true, also emit World-feature markers (Stakes of Marika, Imp Statues,
        // Hero's Tomb, …) sourced straight from the disk MSBs by their AEG asset model —
        // no committed bake. The model→category map is the generated WORLD_FEATURE_MODELS
        // table (tools/world_feature_assets.py); baked twins are dropped at finalize
        // (category-wipe for dedicated categories, cell-dedup for shared ones). See
        // build_disk_world_feature_markers.
        inline constexpr bool worldFeaturesFromDisk = true;
        // When true, drop baked loot markers whose item is sold infinite-stock
        // (sellQuantity == -1) in the live ShopLineupParam. These are merchant items
        // with NO world placement that the bake's unmatched-ItemLotParam fallback put on
        // the map at the tile corner (0,0,0) — a phantom the player can't find. Reads
        // ShopLineupParam live (any mod); only drops a still-baked marker (no disk twin).
        extern bool dropMerchantPhantoms;
        // Directory holding the active mod's map\MapStudio\*.msb.dcx (or the mod
        // root, or a map\ root). Empty = auto-detect: the DLL's own mod folder,
        // then the Elden Ring install dir. Set it to your ModEngine2 mod's map
        // folder if auto-detect picks the wrong source.
        extern std::string lootMsbDir;

        // ── ERR Markers ─────────────────────────────────────────────────
        extern bool redifyBossIcons;  // overlay: boss markers drawn red + auto-hide on kill

        // Grace rendering: when graceOverlay is on, the overlay draws ALL graces itself
        // (discovered = full colour, undiscovered = grey) instead of the hybrid (native draws
        // discovered). graceGpuSprite picks the icon source: false = baked atlas (clean, constant),
        // true = the live engine sprite (SB_ERR_Grace, time-of-day tinted). Needs native-pin
        // suppression to avoid doubling discovered graces.
        extern bool graceOverlay;
        extern bool graceGpuSprite;

        // Suppress the game's native discovered-grace map pins (so the overlay is the sole grace
        // source, paired with graceOverlay). Hooks the WarpPinData builder (RE e4b3f6a). PHASE A:
        // when on, the hook installs + LOGS each grace pin build ([WARPPIN]) to confirm we can
        // identify discovered ones — actual suppression is gated behind this once verified.
        extern bool graceSuppressNative;
        extern bool suppressNativeBosses;

        // Marker dump (hotkey → dump beacon/stamp coords to file)
        extern bool enableMarkerDump;
        extern uint32_t markerDumpKey;  // Win32 VK_* code (default VK_F9 = 0x78)

        // Thread 7 — coverage-gap observers (log to logs/MapForGoblins_events.log).
        // debugEventFlags  = hook SetEventFlag (every flag the game sets).
        // debugItemGrants  = hook AddItemFunc (every inventory grant / pickup).
        // See goblin_debug_events.{hpp,cpp}.
        extern bool debugEventFlags;
        extern bool debugItemGrants;
        // debugFlagCapture = light SetEventFlag hook for the overlay's NPC
        // death-flag capture tool (Quest Browser Part 2).
        extern bool debugFlagCapture;

        // Read-only world-map cursor probe (logs cursor coords to confirm the RE
        // offsets / marker-space). See goblin_worldmap_probe.{hpp,cpp}.
        extern bool debugWorldmapProbe;

        // Dev probe: hook the world-map page-switch handlers and log which fires +
        // its args + the resulting page on each manual page change. Pins which DLC
        // sibling does base<->DLC + confirms args (docs/re/windows_worldmap_page_
        // switch_re_prompt.md). See goblin_worldmap_probe.cpp.
        extern bool debugPageSwitch;

        // Use the engine's own live world->map-space projection (call the native
        // WorldMapViewModel) instead of our baked LEGACY_CONV + affine + DLC eyeball.
        // Fixes dungeon/underground marker placement (proper LegacyConv fold). Falls
        // back to baked when the map is closed / an area isn't placed by the game.
        extern bool liveProjection;

        // Dev probe: hook CSScaleformImageCreator::CreateImage and log each worldmap
        // icon image (sprite rect + backing GPU texture) to crack the iconId↔image
        // mapping for runtime icon textures. See goblin_inject.cpp icon-texture probe.
        extern bool dumpIconTextures;

        // Dev one-shot: find the live CS::WorldMapViewModel + dump its converter
        // array (VM+0xF8) — confirms the world->map-space projection RE before we
        // wire it. See goblin_worldmap_probe.cpp dump_converters_once.
        extern bool dumpConverters;

        // Dev one-shot: walk the native-pin icon manager (CSWorldMapPointMan
        // [er+ICON_MGR_SLOT_RVA] std::map @+0x398) and dump each built pin's key/ins
        // to MapForGoblins.log as [PINS] — identifies WHAT native pins exist + their
        // source before we suppress them (overlay = sole icon source). Read-only
        // (ReadProcessMemory). See goblin_worldmap_probe.cpp dump_native_pins.
        extern bool dumpNativePins;

        // Dev prototype: draw overlay-rendered marker dots projected onto the open
        // world map (verifies the world->screen affine). See goblin_overlay.cpp +
        // goblin_worldmap_probe::get_live_view.
        extern bool overlayMarkersProto;

        // Dev: log ER's render-output dims each ~2s ([RENDIMS]) to diagnose the
        // mid-session resolution-change zoom corruption. Read-only.
        extern bool debugRenderDims;

        // EXPERIMENTAL: on a swapchain resize, raw-poke ER's stale render-output dims
        // to the new size so a mid-session resolution change doesn't leave the world
        // zoomed (no restart needed). Same-aspect only. Default off.
        extern bool fixMidsessionResolution;

        // [BENCH] logging gates (goblin_bench.hpp). Independent — both true by default (matches
        // prior behavior unchanged); set both false to silence [BENCH] entirely. Does NOT affect
        // [BENCH][SPIKE] lag-hitch warnings, which always fire regardless (anomaly alert, not
        // routine noise).
        extern bool benchLogIndividual;  // per-call "[BENCH] label: X ms" lines
        extern bool benchLogSession;     // the "[BENCH] SESSION REPORT" dump at detach

        // Dev RE tool (offset source-of-truth): embedded find-what-accesses filtered to
        // eldenring.exe. Arms a HW breakpoint on a live param row+offset (probeFieldSpec =
        // "ParamName:rowId:offset[:len[:rw]]") and logs [FWA] the game's own access site,
        // skipping every mod read. See goblin_field_probe.{hpp,cpp}. Off by default.
        extern bool probeFieldAccess;
        extern std::string probeFieldSpec;

        // Overlay marker sizes. Final = resolution-base × master × type-scale.
        extern float overlayMasterScale;   // all overlay markers + piles
        extern float overlayIconScale;     // category marker icons
        extern float overlayClusterScale;  // cluster pile glyphs
        extern float graceIconScale;       // grace markers only (calibration)
        extern float mapSymbolScale;       // native MENU_MAP_* map symbols (bosses etc)
        extern bool  iconLegibility;       // DX item 1: clamp min icon size + dark backing disc for contrast
        extern float iconMinHalfPx;        // min icon half-extent (px) when iconLegibility is on
        extern bool  altitudeCue;          // DX item 7: ▲/▼ badge when a marker is above/below the player
        extern float altitudeDeadzone;     // world-Y diff (units) below which no badge is drawn
        extern float graceOffsetX, graceOffsetY;  // overlay grace draw px offset (native-vs-imgui compare)
        extern float viewDelayFrames;      // marker motion-sync delay in present-frames (A/B the pan/zoom re-adjust)
        extern bool  viewDelayZoom;        // motion-sync delay also delays zoom (off = live zoom, fixes wheel-step teleport)

        // Debug viz: cluster pile anchor + member lines + name + d/thr (own toggle).
        extern bool debugClusterAnchors;
        // Debug viz: draw each MapNameOverride region volume + name (red = unresolved).
        extern bool debugRegionVolumes;

        // In-game minimap HUD (corner, north-up, overworld-only). Opt-in.
        extern bool showMinimap;
        extern float minimapZoom;     // px per world-unit
        extern float minimapSize;     // radius px
        extern float minimapOpacity;  // background opacity 0..1
        extern bool minimapAnchorRight;
        extern bool minimapAnchorBottom;
        extern float minimapOffsetX;
        extern float minimapOffsetY;

        // In-game per-section visibility (the 7 display groups). The section_*
        // bools are the persisted runtime state, driven live by the overlay menu
        // (F1) and written back on Save. See goblin_config_schema [Display Sections].
        extern bool sectionEquipment, sectionKeyItems, sectionLoot, sectionMagic,
                    sectionQuest, sectionReforged, sectionWorld;

        // Marker clustering (v1). See goblin_config_schema [Clustering].
        extern bool enableClustering;
        extern bool clusterHard;  // hard = mixed-category piles; soft = per-category
        extern uint8_t clusterThreshold;   // base cluster size; the FAR (clustered) size when distance-adaptive
        // Distance-adaptive clustering: near the player use a HIGH threshold (few
        // piles = detail / real items), ramping DOWN to clusterThreshold far away
        // (more clustering = fewer distant icons).
        extern bool    clusterDistanceAdaptive;
        extern uint8_t clusterNearThreshold; // detail size NEAR player (high = more individual items)
        extern uint8_t clusterNearRadius;    // tiles: full-detail radius
        extern uint8_t clusterFarRadius;     // tiles: at/beyond → clusterThreshold (clustered)
        // Per-category cluster opt-out. Comma-separated category names (loose match,
        // like showAllExcept) that stay EXACT markers and never fold into a cluster.
        // Empty = every category is clusterable (the v1 behaviour).
        extern std::string clusterExclude;
        // Per-category cluster-threshold overrides: "Name:N,Name2:M" (loose name
        // match). A category not listed uses the global clusterThreshold. Driven by
        // the per-category threshold inputs in the overlay.
        extern std::string clusterThresholdOverrides;

        // Thread 1 v1.5 — quest-aware quest-NPC markers. When true, a WorldQuestNPC
        // marker for one of the 34 curated questlines shows only while that quest is
        // active (its event flag set); off = all quest-NPC markers always shown.
        extern bool questNpcQuestAware;

        // Quest Browser per-step progress: one '0'/'1' char per global step index
        // (author order in goblin_quest_steps). Auto-grown; persisted on Save.
        extern std::string questProgress;

        // In-world region chips: one '0'/'1' char per major-region anchor (anchor order;
        // '0' = region hidden). Managed by the overlay map; persisted on Save.
        extern std::string regionToggles;

        // Quest Browser: grey out + tag a questline ([unfinishable]/[concluded])
        // when the overlay reads its NPC's death/conclusion fail_flag as set.
        // Default true (existing behaviour). EXPERIMENTAL — the per-NPC death
        // flags are reverse-engineered and may mis-grey; users can turn it off.
        // Toggle is in the overlay Quest Browser; persisted in the ini.
        extern bool questGreyOnDeath;
    };

    uint32_t parse_vk_code(std::string name);
    uint16_t parse_gamepad_combo(std::string s);
    std::string mask_to_combo_string(uint16_t mask);
};
