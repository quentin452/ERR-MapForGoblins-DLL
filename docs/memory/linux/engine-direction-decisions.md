---
name: engine-direction-decisions
description: Strategic decisions (2026-06-23) on which project to pursue — ER mod vs EvoCraft vs a new separate 2.5D engine; plus the locked architecture/genre calls
metadata: 
  node_type: memory
  type: project
---

Long strategy discussion 2026-06-23. User is weighing whether to keep modding Elden Ring, push
EvoCraft, or start a fresh separate engine. **Reflection in standby — primary not yet chosen.** But
several calls are LOCKED (don't re-litigate):

**Locked decisions:**
- **A new 2.5D + ImGui game engine must be a SEPARATE repo — never inside EvoCraft.** Bolting a casual
  2.5D/ImGui render path onto EvoCraft violates EvoCraft's core doctrines (mesh-shaders-only, GPU-resident
  state, zero `vkCreateGraphicsPipelines`) → it'd be a 2nd engine with opposite philosophy, a permanent
  solo maintenance tax, and it wouldn't use EvoCraft's moat. Keep EvoCraft PURE/mono-purpose.
- **For the combat feel the user loves in Souls (= combat SKILL: dodge/timing/stamina/combos, NOT the
  3rd-person shoulder-cam immersion), 2.5D is enough — no true 3D needed.** Hades is the proof (2.5D iso
  action-roguelite, Souls skill-feel, solo-viable). True 3rd-person *camera* immersion would force 3D
  (Godot/Unreal); the user does NOT need that.
- **Reuse boundary:** one tech base spawns many games only WITHIN a genre family. A 2D-grid/2.5D-plane
  engine → roguelike/tactics/ARPG/twin-stick/Hades-like (big reuse). It will NEVER become a 3D Souls-like
  (≈0 reuse — different sim core entirely).

**The new-engine plan (if chosen):** 2.5D rendering (iso or top-down perspective) + a CONTINUOUS
real-time plane sim (fixed 60Hz tick, float positions, hitbox/hurtbox, dodge+i-frames, stamina, frame-
based attacks startup/active/recovery, telegraphed AI). Reuses MapForGoblins bricks: DX12 device/
swapchain, atlas sprites (stb), ImGui for HUD/menus, config/save, projection→camera. NEW work = the
combat sim + depth-sorted 2.5D render. First milestone = a "Combat Room" vertical slice (1 player vs 1
enemy: move/attack/dodge/stamina/hitbox/hitstun) — proves the only hard part (the feel) before content.
ImGui perf: fine for UI always; for tiles/sprites a small instanced DX12 sprite-batcher (1 atlas, 1 draw
for 10k+ quads) beats ImGui-DrawList-everything if scaling up — but ImGui-everything is fine to prototype.

**The 3 options (META-RULE: pick ONE primary, others standby — three in parallel kills the solo dev):**
- **ER mod (MapForGoblins)** → optimizes FINISH/polish; lives + works, but a version-break treadmill +
  RE thrash (see [[overlay-rendered-markers]], [[loot-identity-stable-err-additive]]).
- **EvoCraft** (`~/Documents/GitHub/EvoCraft/`) → optimizes CRAFT/research; the moat, long-haul, niche
  audience (mesh-shaders/BDA/shader_object = modern-GPU-only, "elite").
- **New 2.5D engine** → optimizes SHIPPING a product the user owns end-to-end, broad PC reach.

Decision pending — user to choose the primary.
