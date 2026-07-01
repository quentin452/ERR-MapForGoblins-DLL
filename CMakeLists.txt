cmake_minimum_required(VERSION 3.28.1)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
  set(CMAKE_GENERATOR_PLATFORM x64)
endif()

project( MapForGoblins
  VERSION   "1.0.18"
  LANGUAGES CXX RC)

# The VERSIONINFO resource (.rc) is preprocessed by clang-cl, which must find
# <winver.h> from the xwin SDK (the CXX /imsvc dirs don't apply to the RC rule).
if(DEFINED XWIN)
  string(APPEND CMAKE_RC_FLAGS
    " -I ${XWIN}/sdk/include/um -I ${XWIN}/sdk/include/shared -I ${XWIN}/sdk/include/ucrt")
endif()

if(NOT DEFINED VERSION_PRE)
  set(VERSION_PRE "pre")  # default for dev/snapshot; build.bat release passes -DVERSION_PRE=""
endif()

# Which baked-data dir to compile. Default = ERR (src/generated). The vanilla
# build passes -DGENERATED_SUBDIR=generated_vanilla so the two bakes never
# clobber each other. Sources include the generated headers by bare name and
# resolve them via the include path below.
if(NOT DEFINED GENERATED_SUBDIR)
  set(GENERATED_SUBDIR "generated")
endif()
set(GEN_DIR "${CMAKE_SOURCE_DIR}/src/${GENERATED_SUBDIR}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

include(FetchContent)

FetchContent_Declare(minhook
  GIT_REPOSITORY        https://github.com/TsudaKageyu/minhook.git
  GIT_TAG               f5485b8454544c2f034c78f8f127c1d03dea3636)

FetchContent_Declare(pattern16
  GIT_REPOSITORY        https://github.com/Dasaav-dsv/Pattern16.git
  GIT_TAG               728eac543cee7e7bf6fda445d7db06e3dc8a61d0
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

FetchContent_Declare(mini
  GIT_REPOSITORY        https://github.com/metayeti/mINI.git
  GIT_TAG               0.9.15
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

set(SPDLOG_DISABLE_DEFAULT_LOGGER ON)
set(SPDLOG_USE_STD_FORMAT ON)
FetchContent_Declare(spdlog
  GIT_REPOSITORY        https://github.com/gabime/spdlog.git
  GIT_TAG               v1.13.0)

# Dear ImGui (in-game settings overlay, Thread 6). No CMakeLists of its own —
# we add its sources straight into the target below. Pinned to v1.90.9, whose
# imgui_impl_dx12 uses the classic single-font-SRV Init signature.
FetchContent_Declare(imgui
  GIT_REPOSITORY        https://github.com/ocornut/imgui.git
  GIT_TAG               v1.90.9
  CONFIGURE_COMMAND     ""
  BUILD_COMMAND         "")

# Set iterator debug level to 0 for ELDEN RING ABI compatibility
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# Static CRT linking (/MT) - required for DLL injection into game process
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

FetchContent_MakeAvailable(minhook pattern16 mini spdlog imgui)

add_library(pattern16 INTERFACE)
target_include_directories(pattern16 INTERFACE ${pattern16_SOURCE_DIR}/include)

add_library(mini INTERFACE)
target_include_directories(mini INTERFACE ${mini_SOURCE_DIR}/src)

# Dear ImGui core + DX12/Win32 backends, compiled into the DLL.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)

add_library( MapForGoblins SHARED
  src/from/params.hpp
  src/from/params.cpp
  src/from/paramdef/WORLD_MAP_POINT_PARAM_ST.hpp
  src/from/paramdef/WORLD_MAP_LEGACY_CONV_PARAM_ST.hpp
  src/goblin_legacy_fold.hpp
  src/goblin_legacy_fold.cpp
  src/modutils.hpp
  src/modutils.cpp
  src/goblin_log_archive.hpp
  src/goblin_log_archive.cpp
  src/goblin/goblin_structs.hpp
  src/goblin/goblin_map_flags.hpp
  src/goblin/goblin_map_tiles.hpp
  src/goblin/goblin_map_exceptions.hpp
  ${GEN_DIR}/goblin_map_data.hpp
  ${GEN_DIR}/goblin_map_data.cpp
  ${GEN_DIR}/goblin_category_exceptions.hpp
  ${GEN_DIR}/goblin_category_exceptions.cpp
  ${GEN_DIR}/goblin_quest_gates.hpp
  ${GEN_DIR}/goblin_quest_gates.cpp
  ${GEN_DIR}/goblin_quest_steps.hpp
  ${GEN_DIR}/goblin_quest_steps.cpp
  ${GEN_DIR}/goblin_name_aliases_en.hpp
  ${GEN_DIR}/goblin_name_aliases_en.cpp
  ${GEN_DIR}/goblin_region_anchors.hpp
  ${GEN_DIR}/goblin_region_anchors.cpp
  ${GEN_DIR}/goblin_tile_tabs.hpp
  ${GEN_DIR}/goblin_tile_tabs.cpp
  ${GEN_DIR}/goblin_major_regions.hpp
  ${GEN_DIR}/goblin_major_regions.cpp
  ${GEN_DIR}/goblin_name_regions.cpp
  ${GEN_DIR}/goblin_world_feature_models.hpp
  ${GEN_DIR}/goblin_world_feature_models.cpp
  src/goblin_config_schema.hpp
  src/goblin_config_schema.cpp
  src/goblin_config.hpp
  src/goblin_config.cpp
  src/goblin_collected.hpp
  src/goblin_collected.cpp
  src/goblin_debug_events.hpp
  src/goblin_debug_events.cpp
  src/goblin_field_probe.hpp
  src/goblin_field_probe.cpp
  src/goblin_worldmap_probe.hpp
  src/goblin_worldmap_probe.cpp
  src/goblin_inject.hpp
  src/goblin_inject.cpp
  src/goblin_inject_shared.hpp
  src/goblin_loot_resolve.cpp
  src/goblin_icon_harvest.cpp
  src/goblin_item_classify.cpp
  src/goblin_section_visibility.cpp
  src/goblin_kindling.hpp
  src/goblin_kindling.cpp
  src/goblin_logic.hpp
  src/goblin_logic.cpp
  src/goblin_markers.hpp
  src/goblin_markers.cpp
  src/goblin_messages.hpp
  src/goblin_messages.cpp
  src/goblin_overlay.hpp
  src/goblin_overlay.cpp
  src/input/input_shared.hpp
  src/input/input_directinput.hpp
  src/input/input_directinput.cpp
  src/input/input_gamepad.hpp
  src/input/input_gamepad.cpp
  src/input/input_cursor.hpp
  src/input/input_cursor.cpp
  src/input/input_rawinput.hpp
  src/input/input_rawinput.cpp
  src/input/input_wndproc.hpp
  src/input/input_wndproc.cpp
  src/input/input_keyboard_poll.hpp
  src/input/input_keyboard_poll.cpp
  src/goblin_projection.hpp
  src/generated_shared/goblin_overlay_icons.hpp
  src/generated_shared/goblin_overlay_icons.cpp
  src/stb_image_impl.cpp
  src/worldmap/marker_layer.hpp
  src/worldmap/grace_layer.hpp
  src/worldmap/grace_layer.cpp
  src/worldmap/quest_npc_layer.hpp
  src/worldmap/quest_npc_layer.cpp
  src/worldmap/category_meta.hpp
  src/worldmap/category_meta.cpp
  src/worldmap/map_entry_layer.hpp
  src/worldmap/map_entry_layer.cpp
  src/worldmap/map_renderer.hpp
  src/worldmap/map_renderer.cpp
  src/worldmap/msbe_parser.hpp
  src/worldmap/msbe_parser.cpp
  src/worldmap/loot_disk.hpp
  src/worldmap/loot_disk.cpp
  src/worldmap/dvdbnd_reader.hpp
  src/worldmap/dvdbnd_reader.cpp
  src/worldmap/loot_open_probe.hpp
  src/worldmap/loot_open_probe.cpp
  src/goblin_crashdump.cpp
  src/dllmain.cpp)

set_target_properties(MapForGoblins PROPERTIES OUTPUT_NAME "MapForGoblins")

# Non-ERR bakes (vanilla, convergence): omit + disable ERR-only config
# entries at runtime. MFG_VANILLA here means "no ERR-only features".
if(NOT GENERATED_SUBDIR STREQUAL "generated")
  target_compile_definitions(MapForGoblins PRIVATE MFG_VANILLA=1)
endif()

# Vanilla-ONLY (not Convergence): used to default the live-loot / randomizer-
# compat options ON, since the plain vanilla build is what randomizer players
# use. ERR and Convergence keep them OFF (opt-in).
if(GENERATED_SUBDIR STREQUAL "generated_vanilla")
  target_compile_definitions(MapForGoblins PRIVATE MFG_PROFILE_VANILLA=1)
endif()

# Tiny build-time tool that emits the default ini from the in-code schema
# (src/goblin_config_schema.cpp). build.bat runs it to generate the shipped
# ini per profile instead of copying a hand-maintained file.
add_executable(mfg_inigen src/inigen_main.cpp src/goblin_config_schema.cpp)
target_include_directories(mfg_inigen PRIVATE ${CMAKE_SOURCE_DIR}/src)
# schema.cpp now includes goblin_map_data.hpp (the Category enum) for the
# showCategory[] array — give the generator the active bake's include dir too.
target_include_directories(mfg_inigen PRIVATE ${GEN_DIR})
# Keep the generated ini's defaults in sync with the DLL's compiled defaults.
if(GENERATED_SUBDIR STREQUAL "generated_vanilla")
  target_compile_definitions(mfg_inigen PRIVATE MFG_PROFILE_VANILLA=1)
endif()

if(VERSION_PRE)
  set(FULL_VERSION "${VERSION_PRE}-${CMAKE_PROJECT_VERSION}")
else()
  set(FULL_VERSION "${CMAKE_PROJECT_VERSION}")
endif()
configure_file(${CMAKE_SOURCE_DIR}/src/version.h.in
               ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
# Win32 VERSIONINFO resource (CompanyName/FileDescription/versions) so the DLL
# carries metadata → fewer antivirus false positives on the unsigned binary.
configure_file(${CMAKE_SOURCE_DIR}/src/version.rc.in
               ${CMAKE_BINARY_DIR}/generated/version.rc @ONLY)
target_sources(MapForGoblins PRIVATE ${CMAKE_BINARY_DIR}/generated/version.rc)
target_include_directories(MapForGoblins PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(MapForGoblins PRIVATE ${CMAKE_BINARY_DIR}/generated)
# Resolve bare `#include "goblin_map_data.hpp"` etc. to the active bake dir.
target_include_directories(MapForGoblins PRIVATE ${GEN_DIR})
# stb_image.h (single-header PNG decode for the embedded icon atlas).
target_include_directories(MapForGoblins PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

# MapForGoblins.ini is no longer copied: build.bat generates it per profile in
# :package via mfg_inigen, and the DLL self-creates/repairs it at runtime
# (goblin::ensure_ini), so dev runs need no shipped ini. Keep the dependency so
# a plain `build.bat` also builds the generator.
add_dependencies(MapForGoblins mfg_inigen)

target_link_libraries( MapForGoblins
  minhook
  pattern16
  mini
  spdlog
  imgui
  d3d12
  dxgi
  dinput8
  dxguid
  dbghelp
  bcrypt)
