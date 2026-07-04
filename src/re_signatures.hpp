#pragma once
//
// Central registry of AOB signatures + image RVAs the mod resolves at runtime.
//
// WHY THIS FILE EXISTS: every ELDEN RING / ERR update can shift function bodies
// and move static singleton slots, breaking these. Keeping them in ONE place means
// "what broke after the patch?" is a single file to audit — and `resolve_all_signatures()`
// (below) logs PASS/FAIL for each at startup, so the answer is one glance at the log.
//
// AOBs are PATCH-RESILIENT (we pin code signatures, never hardcoded RVAs) but the
// signature bytes themselves can still drift. Each entry notes what it resolves and a
// re-find hint. `relative_offsets` (e.g. {{3,7}} to extract a rip-relative slot) stay at
// the call sites — this file holds the byte strings + the few genuine RVAs only.
//
#include <cstdint>

#include "modutils.hpp"
#include "spdlog/spdlog.h"

namespace goblin::sig
{
    // ── EventFlag subsystem (read + write the game's event-flag bitmap) ──
    // IsEventFlag(EventFlagMan*, u32* id) -> bool. Getter; NO r8b value-capture
    // (that distinguishes it from the setter). From Erd-Tools.
    inline constexpr const char *IS_EVENT_FLAG = "48 83 EC 28 8B 12 85 D2";
    // EventFlagMan singleton slot: `mov rdi,[rip+disp]; test rdi,rdi; …; xor al,al; jmp`.
    // Use with relative_offsets {{3,7}} to lift the slot from the rip-disp.
    inline constexpr const char *EVENT_FLAG_MAN_SLOT =
        "48 8B 3D ?? ?? ?? ?? 48 85 FF ?? ?? 32 C0 E9";
    // SetEventFlag ("EventFlag_C1") ENTRY. CAREFUL: a sibling fn shares the prologue +
    // `8B 12 48 8B F1 85 D2`, then DIVERGES — the REAL SetEventFlag has a short `74` (jz)
    // there; the DECOY has `0F 84` (jz near). Stop the signature at the `74` to pin the real
    // one UNIQUELY (the prologue-only 22-byte form matched BOTH → resolved to the wrong fn
    // ~half the time). The full Hexinton AOB (`…85 D2 0F 84`) actually describes the DECOY
    // and fails at runtime. Verified live: this resolves the real entry (0x1405d2240,
    // read-back 5/5 OK); the `7D` rel8 after `74` is excluded as version-variant.
    inline constexpr const char *SET_EVENT_FLAG =
        "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 30 48 8B DA 41 0F B6 F8 "
        "8B 12 48 8B F1 85 D2 74";
    // Alt EventFlagMan singleton resolve used by the inject fast-path (fe_man_slot).
    inline constexpr const char *EVENT_FLAG_MAN_SLOT_ALT =
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 11 8B 80 3C 65 00 00";

    // ── Item grants (Thread 7 coverage observer) ──
    // AddItemFunc / ItemGib prologue.
    inline constexpr const char *ADD_ITEM_FUNC =
        "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC "
        "90 01 00 00 48 C7 45 C8 FE FF FF FF 48 89 9C 24 D8 01 00 00 48 8B 05";
    // MapItemMan static accessor (the `inv` rcx arg for a framework GRANT / sidecar strip).
    // From the Hexinton CT ItemGib ("Zodiacsl125"). The trailing `48 8B 0D` (mov rcx,[rip+disp32])
    // sits at AOB+0x16: slot = (match + 0x1D) + *(int32*)(match + 0x19); inv = *slot. Static +
    // version-resilient — supersedes the AddItemFunc-observer-captured pointer (which needed a
    // prior game grant). Call convention (also from the CT): AddItemFunc(rcx=inv, rdx=&entry
    // {qty:i32@0, id:u32@4}, r8=scratch buffer base, r9=0). Removal = negative qty (ER has no
    // separate RemoveItem — the CT, a full trainer, exposes none).
    inline constexpr const char *INVENTORY_ACCESSOR =
        "44 8B 61 1C 41 8B FC C1 EF 07 40 80 E7 01 41 C1 EC 08 41 80 E4 01 48 8B 0D";
    // GameDataMan static accessor (Hexinton CT) → the player's persistent game data (the SAVE
    // serialize snapshots this). slot = match+7+*(int32*)(match+3); GameDataMan = *slot.
    // Chain: GameDataMan → +0x8 PlayerGameData → +0x2B0 EquipGameData (the inventory the serialize
    // reads — sidecar Phase-2 strip bracket target, docs/re/windows_save_function_rpm_re_findings.md).
    // Unique on the ERR-Steam exe (offline scan 2026-07-03 @ 0x255b90).
    inline constexpr const char *GAME_DATA_MAN =
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3";

    // ── Save routine (sidecar Phase-2) — CS::SaveLoad2::SLSaveSession::run ──
    // The ER save subsystem is CS::SaveLoad2 (RTTI-mapped via live RPM, Windows box 2026-07-03:
    // docs/re/windows_save_function_rpm_re_findings.md). This is `SLSaveSession::run` (vtable slot 2,
    // RVA 0x240FD70 on the Windows App 2.6.x build) — the OUTER save-WRITE state machine, reached by
    // VTABLE dispatch through the SLSaveSession vtable (which is why the Linux rel32 stack walk's
    // 0x253e4b0 guess got 0 observer calls — it was never E8-called). Convention: rcx=this
    // (SLSaveSession*), no other args; stack-canary framed. Prefer RTTI resolution (TD
    // `.?AVSLSaveSession@SaveLoad2@@` -> COL -> vtable[2]); this AOB is the fallback.
    // ⚠ WRITE side only: SaveLoad2 never reads GameDataMan — it writes an already-serialized content
    // buffer, so hooking this is TOO LATE to strip inventory. Good as a save-detection observer /
    // write bracket; the strip/reinject bracket needs the game-data SERIALIZE (find-what-accesses
    // EquipGameData — the one RE step left). VERIFIED UNIQUE on the ERR-Steam exe (offline scan
    // 2026-07-03: 1 match @ file_off 0x240f370 = RVA 0x240FD70) → [SIG] will PASS on this build.
    inline constexpr const char *SAVE_FN =
        "48 8B C4 55 57 41 54 41 56 41 57 48 8D 68 A1 48 81 EC B0 00 00 00 48 C7 45 EF FE FF FF FF 48 89 58 10";

    // ── Game-data SERIALIZE (sidecar Phase-2 strip/reinject bracket) ──
    // The whole-slot game-data SERIALIZE (RVA 0x67dc00) — found in Ghidra via find_serialize.java
    // (GameDataMan-xref ∩ DLOutputStream-writer), docs/re/windows_save_serialize_re_findings.md.
    // Builds a DLOutputStream over the out-buffer, writes header → FUN_140257f20 (player data incl.
    // inventory: reads GameDataMan→PlayerGameData live) → ~11 more section serializers → trailer, then
    // seeks back and patches the size header. SAVE-SPECIFIC (write-only DLOutputStream — no save/load
    // ambiguity) and SYNCHRONOUS (direct calls, not vtable-dispatched). Convention:
    //   FUN_14067dc00(rcx=save_ctx, rdx=out_buffer, r8d=buf_size, r9=&out_written) -> bool.
    // Strip at ENTRY / reinject at EXIT = the atomic bracket (inventory is serialized INSIDE, after
    // entry → the written buffer excludes the custom item; reinject after exit restores it live).
    // Supersedes every prior wrong guess: 0x253e4b0 (SAVE_FN write side), 0x2573c0 (boot/load, 0 save
    // calls), 0x1ede700 (a DLOutputStream primitive). ⚠ Before flipping kItemStripReinjectWired: hook
    // READ-ONLY first (observer) to confirm it fires once-per-save on a real save + note the thread;
    // and re-verify [SIG] on the ERR deploy build (AOB from the Windows App 2.6.x build).
    inline constexpr const char *SERIALIZE_FN =
        "40 55 53 56 57 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 C7 45 A0 FE FF FF FF";

    // ── Grace warp (fast-travel to a site of grace — dev-world navigation) ──
    // From the Hexinton CT ("Coinsworth"). LuaWarp_01 = the game's Lua-event warp: proper
    // area-load fast-travel. AOB anchors on the previous fn's `C3` ret; the callable is
    // MATCH + 2. Convention: LuaWarp_01(rcx=[CSLuaEventManager+0x18], rdx=[CSLuaEventManager+0x08],
    // r8d = grace id). graceId = the bonfire entity id (e.g. 1042362951 = The First Step, 10002951 =
    // Margit). NOTE: the CT documented r8d = graceId-1000, but that lands ONE BONFIRE OFF (same map
    // cell); ground truth (2026-07-04) is the bonfireEntityId DIRECTLY (offset 0).
    inline constexpr const char *LUA_WARP =
        "C3 ?? ?? ?? ?? ?? ?? 57 48 83 EC ?? 48 8B FA 44";
    // CSLuaEventManager singleton (mov rax,[rip+disp]; disp@+3, instr len 7 → slot = match+7+disp).
    // The bare `…44 89 75` form matched TWO sites on ERR (0x65a1bd + 0xbbaf0b); the CORRECT one
    // (grace warp verified live 2026-07-03, [SIG] 0x…525abbd = er+0x65abbd) continues `?? 48 8B 48 08`
    // (mov rcx,[rax+8]) while the decoy continues `?? 48 83 78 08` (cmp) — so the trailing `?? 48 8B`
    // pins it UNIQUE (offline exe scan confirmed 1 match). Was MULTI, now unique.
    inline constexpr const char *CS_LUA_EVENT_MANAGER =
        "48 8B 05 ?? ?? ?? ?? 48 85 C0 74 ?? 41 BE 01 00 00 00 44 89 75 ?? 48 8B";

    // ── Live param + message repositories (SoloParamRepository / MsgRepositoryImp) ──
    // Param list address (get_param). relative_offsets {{3,7}}.
    inline constexpr const char *SOLO_PARAM_LIST =
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 45 33 C0 BA 90";
    // MsgRepositoryImp singleton. relative_offsets {{3,7}}.
    inline constexpr const char *MSG_REPOSITORY =
        "48 8B 3D ?? ?? ?? ?? 44 0F B6 30 48 85 FF 75";
    // MsgRepositoryImp::GetMessage(repo, group, fmgId, msgId) -> wchar_t* (NULL on miss).
    // FUN_14266d3c0 @ RVA 0x266d3c0; RCX=repo EDX=group(=0) R8D=fmgId(=slot) R9D=msgId.
    // Internally bounds- and null-checks (fmgId<count2, base_array[group][fmgId]) then
    // FmgFile_lookup — so calling it is crash-safe even on ERR's stub DLC slots, and on
    // ERR it returns base+DLC-merged content (ERR hooks this fn). Lets us resolve names
    // WITHOUT hand-walking slot internals → kills #ifdef MFG_VANILLA + copy_fmg_*.
    // The AOB anchors on the INTERIOR (entry+5): under ERR the first 5 prologue bytes are
    // overwritten by ERR's own MinHook E9 trampoline, so an entry-prologue AOB fails.
    // Resolve as: entry = (match address) - 5.  RE: docs/re/windows_native_msg_getter_re_findings.md
    inline constexpr const char *GETMESSAGE =
        "44 3B 41 14 73 ?? 48 8B 41 08 8B D2 48 8B 0C D0 48 85 C9 74 ?? 41 8B C0 "
        "48 8B 0C C1 48 85 C9 74 ?? 41 8B D1 E9";

    // ── Param FIELD-OFFSET access sites (read the offset live from the game's code) ──
    // The "industrial offset-free" mechanism: instead of hardcoding `row->b[0x3e]`, AOB the game's
    // own field-access instruction and read the offset out of its ModRM displacement at init — the
    // value is then self-correcting across patches (memory param-offset-source-of-truth; prototype +
    // authoring tool D:\ghidra_scripts\offset_resolver.py). Use with modutils::resolve_field_offset.
    //
    // EquipParamGoods.goodsType: read site `cmp byte ptr [rcx+0x3e],0Dh` (a goodsType==13 category
    // check) guarded by `test rcx,rcx; je`. PARAM-ANCHORED + verified live (CE find-what-accesses:
    // RCX = goods row 0x…2c150, RDX = EquipParamGoods table 0x…00090). The disp8 at AOB position 7
    // IS goodsType's offset. Jump rel8 + disp wildcarded → only opcodes/ModRM/the semantic imm 0x0D
    // remain (patch-stable). Authored 2026-06-26 (unique in .text). disp_pos=7, disp_size=1.
    inline constexpr const char *GOODS_TYPE_ACCESS = "48 85 C9 74 ?? 80 79 ?? 0D";

    // EquipParamGoods.sortGroupId: read site `movzx ebx, byte ptr [rax+0x72]` (er+0x86eeeb) — the
    // inventory item-grouping read. Found by the embedded find-what-accesses (goblin_field_probe):
    // RAX = goods row 10105 (verified live); the 72 static lookalikes couldn't be disambiguated, the
    // HW-breakpoint pinned THE one. disp8 at AOB position 3 IS sortGroupId's offset. The two short-jump
    // rel8s are wildcarded so only opcodes + the semantic `mov bl,0xFF` (not-found default) remain.
    // Authored 2026-06-26 (unique in .text). disp_pos=3, disp_size=1.
    inline constexpr const char *GOODS_SORT_GROUP_ACCESS = "0F B6 58 ?? EB ?? B3 FF 85 FF";

    // AssetEnvironmentGeometryParam.pickUpItemLotParamId: read site `mov eax,[rax+0xb8]` (er+0x6c4c11)
    // — the asset-pickup lot resolve. Found by the embedded find-what-accesses (goblin_field_probe)
    // by looting a Runic Trace (AEG099_821 → row 99821); RAX = the AEG row (verified live, +0xb8 =
    // 998210). Static scan was hopeless here (6507 `[reg+0xb8]` reads, none param-anchored). disp32 at
    // AOB position 2 IS pickUpItemLotParamId's offset; the jns rel8 is wildcarded. Authored 2026-06-26
    // (unique in .text). disp_pos=2, disp_size=4.
    inline constexpr const char *AEG_PICKUP_LOT_ACCESS = "8B 80 ?? ?? ?? ?? 85 C0 79 ?? 48 8B 43 08";

    // AssetEnvironmentGeometryParam.isEnableRepick: the repick-eligibility read site
    // `movzx eax,[rax+0x3c]; shr eax,5; and eax,1` (er+0x6c4c59) — extracts BIT 5 of byte 0x3c. The
    // multi-hit field-probe surfaced 7 distinct sites reading byte 0x3c (one per bitfield bit: bit0/3/
    // 6/7…); THIS is the bit-5 one = isEnableRepick. It LIVE-RE-CONFIRMS the 16k-leak fix (bit 5, not
    // the Paramdex's bit 6). Both the field offset AND the bit are read from this instruction: the disp8
    // at AOB position 3 (=0x3c) and the `shr` immediate at position 6 (=5). The disp is wildcarded (so
    // the offset resolves live); the `05` is KEPT to pin THE bit-5 site (the bit-6 sibling is otherwise
    // byte-identical). Trailing `C3 C3` = the accessor epilogue (stable). Authored 2026-06-26 (unique).
    inline constexpr const char *AEG_REPICK_BIT_ACCESS = "0F B6 40 ?? C1 E8 05 83 E0 01 C3 C3";

    // BonfireWarpParam.textId1: read via a GENERIC switch-dispatcher text getter — the cases read
    // consecutive text fields `mov reg,[base+0x30]` (textId1) / `+0x3c` / `+0x48`, and this exact
    // dispatcher shape is REUSED across several param types that share the text block, so the AOB
    // matches 4 sites — all carrying disp 0x30 (resolve with consensus=true). Found by the embedded
    // find-what-accesses (looking at "The First Step" grace row 61423601 textId1 in the warp menu).
    // disp8 at AOB position 3 = textId1's offset (the 0x3c/0x48 cases anchor the field block; the
    // `EB 2b/25/1f` are the per-case jmps to the common tail). NOT in the SIG table (intentionally
    // multi-match; the health check flags MULTI as a bug). Authored 2026-06-26. disp_pos=3, size=1.
    inline constexpr const char *BONFIRE_TEXTID1_ACCESS =
        "41 8B ?? ?? EB 2B 41 8B ?? 3C EB 25 41 8B ?? 48 EB 1F";

    // NOTE — ItemLotParam.getItemFlagId (+0x80) was investigated for runtime-resolution but its read
    // site is a generic tiny getter (`mov eax,[row+0x80]; cmp eax,-1; cmove; ret`) BYTE-IDENTICAL to
    // its 0x84-field sibling, so no disp-wildcarded AOB disambiguates it (the offset is the only diff —
    // circular; consensus fails too since they read different fields). Left PINNED at 0x80 (live-
    // confirmed via the embedded find-what-accesses + guarded by tools/check_param_offsets.py).

    // ── World geometry / collected-state (goblin_collected, goblin_inject map-pos) ──
    // GeomFlagSaveDataManager slot (was RVA 0x3D69D18).
    inline constexpr const char *GEOM_FLAG_SLOT =
        "48 8B 3D ?? ?? ?? ?? 33 F6 48 85 FF 74 ?? 48 8B CF E8 ?? ?? ?? ?? 4C 8B 07";
    // CSWorldGeomMan / map-pos manager slot (was RVA 0x3D69BA8). Shared by collected + inject.
    inline constexpr const char *WORLD_GEOM_MAN_SLOT =
        "48 8B 0D ?? ?? ?? ?? 48 8D 53 10 E8 ?? ?? ?? ?? 4C 8B E8";
    // Geom asset-request manager singleton (pivot-2 ADD path — spawn an AEG asset by name). FD4Singleton
    // DAT_143d69ba8 load site (`mov rcx,[rip+disp]; test rcx,rcx; jz` — the null-check idiom).
    // relative_offsets {{3,7}} lifts the rip-disp → &singleton; then reqMgr = *(void**)(singleton + 0x30)
    // = FUN_1406a5080's param_1. Found via pyghidra byte-scan (b16b21a), verified UNIQUE image-wide;
    // GEOM_REQ_MGR_BACKUP loads the SAME singleton at another site. RE:
    // docs/re/windows_geom_spawn_pivot2_re_findings.md.
    inline constexpr const char *GEOM_REQ_MGR =
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 16 B8 01 00 00 00 48 8D 53";
    inline constexpr const char *GEOM_REQ_MGR_BACKUP =
        "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 14 83 CB 02 89 5C 24 20 48";

    // WorldChrMan finder (player map-pos path).
    inline constexpr const char *WCM_FINDER = "48 8B FA 0F 11 41 70 48 8B 05";
    // Player-MapId singleton load site (was 0x3d691d8). relative_offsets {{3,7}}.
    inline constexpr const char *PLAYER_MAPID_SLOT =
        "48 8B 0D ?? ?? ?? ?? 48 8D 54 24 20 E8 ?? ?? ?? ?? F2 0F 10 05 ?? ?? ?? ??";

    // ── Map markers (beacon/stamp array chain) ──
    inline constexpr const char *MARKER_CHAIN_SLOT =
        "48 8B 0D ?? ?? ?? ?? 48 8B 49 30 48 8D 55 5F";
    inline constexpr const char *MARKER_ARRAY_CTOR =
        "48 8D 05 ?? ?? ?? ?? 48 89 07 48 8D 5F 10 48 8D 05 ?? ?? ?? ??";

    // ── Menu / WorldMapPoint (inject toast + point-man) ──
    // CSMenuMan singleton (toast + worldmap-point paths).
    inline constexpr const char *CSMENUMAN_SLOT = "48 8B 05 ?? ?? ?? ?? 33 DB 48 89 74 24";
    // WorldMapPoint show/select fn.
    inline constexpr const char *WORLDMAP_POINT_FN =
        "48 8B 05 ?? ?? ?? ?? 8B D1 48 85 C0 74 17 48 8B 88 80 00 00 00 48 85 C9";
    // CSWorldMapPointIns / point-man ctor (this=PointMan, ctx=rdx).
    inline constexpr const char *WORLDMAP_POINT_CTOR =
        "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 "
        "48 C7 45 D0 FE FF FF FF 4C 8B F9 8B 42 34";

    // ── Enemy HP-bar (mob NAMES on the game's native health bar) ──
    // RE: docs/re/linux_enemy_healthbar_name_re_findings.md. Struct offsets + these AOBs are derived
    // from the bundled, WORKING PostureBarMod.dll (Mordrog) → live-valid on this ERR/ER build.
    // CSFeManImp singleton slot (mov rcx,[rip+disp]; disp @+3, instr len 7 → relative_offsets{{3,7}}).
    // CSFeManImp+0x59F0 = EntityHpBar entityHpBars[8]; +0x5BF0 = BossHpBar bossHpBars[3].
    inline constexpr const char *CSFEMAN_SLOT =
        "48 8B 0D ?? ?? ?? ?? 8B DA 48 85 C9 75 ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C8 "
        "4C 8D 05 ?? ?? ?? ?? BA B4 00 00 00 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
        "8B D3 E8 ?? ?? ?? ?? 48 8B D8";
    // GetChrInsFromHandle(WorldChrMan*, u64* handlePtr) -> ChrIns*. Matched address IS the function
    // (no RIP deref). ChrIns+0x60 = npcParam (the npcParamId → name).
    inline constexpr const char *GET_CHRINS_FROM_HANDLE =
        "48 83 EC 28 E8 17 FF FF FF 48 85 C0 74 08 48 8B 00 48 83 C4 28 C3";

    // ── World→map-space projection (the native map's per-icon projection) ──
    // RE: docs/re/windows_world_to_mapspace_projection_re_findings.md (FUN_140876140
    // + the live WorldMapViewModel converter array; binary-verified objdump §6b).
    // These let us call/replicate the engine's own projection and delete the baked
    // LEGACY_CONV / -7040+16512 affine / DLC eyeball / marker_group_from. ADDED as a
    // RUNTIME-RESOLUTION TEST first (the [SIG] log proves they resolve live before we
    // wire them). All 4 unique in .text (static AOB check 2026-06-22).
    // FUN_140876140: project ONE point through ONE converter (the affine + Z-flip).
    inline constexpr const char *WORLDMAP_PROJ_POINT =
        "40 53 57 48 83 EC 68 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 50 "
        "41 8B 41 08 48 8B D9 48 8B 49 28 48 8B FA";
    // FUN_1408877d0: loop wrapper (VM, &outMapXZ, &packedId, &worldLocal) → first match.
    inline constexpr const char *WORLDMAP_PROJ_LOOP =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 "
        "48 83 EC 20 33 DB 4D 8B F9 4D 8B E0 4C 8B EA";
    // FUN_140876100: converter-entry builder (field layout of the 0x30-byte converter).
    inline constexpr const char *WORLDMAP_CONV_BUILD =
        "8B 02 89 41 08 F2 41 0F 10 00 F2 0F 11 41 0C 41 8B 40 08 "
        "F3 0F 10 44 24 28 89 41 14 49 8B 01 48 89 41 18";
    // FUN_1408775e0: live WorldMapLegacyConvParam fold (RB-tree remap + pos translate).
    inline constexpr const char *WORLDMAP_LEGACY_FOLD =
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 30 "
        "48 8B F1 49 8B D9 49 8B C9 49 8B F8 4C 8B F2";
    // CS::WorldMapViewModel vtable RVA (the converter array lives at VM+0xF8, count
    // VM+0x280; page table at vtable+0x18 = 0x2ad82f8 = [00 01 0a]).
    inline constexpr uintptr_t WORLDMAP_VIEWMODEL_VTABLE_RVA = 0x2ad82e0;

    // ── Runtime icon textures (read the engine's live worldmap icon sprites) ──
    // RE: docs/re/windows_runtime_icon_textures_re_findings.md. CSScaleformImageCreator::
    // CreateImage (FUN_140d6bbc0): builds each GFx image from the TPF import → a
    // CS::CSTextureImage carrying the sprite RECT (+0x74/+0x7c/+0x3c/+0x40, dims +0x2c/+0x30)
    // + backing GXTexture2D (+0x38 → +0x40 = ID3D12Resource). Hook it to capture the worldmap
    // icon sheet + per-sprite rects, then map to iconIds. Unique in .text (static AOB check).
    inline constexpr const char *WORLDMAP_CREATE_IMAGE =
        "48 8B C4 55 48 8D 68 A1 48 81 EC E0 00 00 00 48 C7 45 A7 FE FF FF FF "
        "48 89 58 08 48 89 78 10";

    // ── Kindling (grace/EcTest/WorldSfx) ──
    // EcTestDistance vftable (was RVA 0x2A5BB90).
    inline constexpr const char *EC_TEST_DISTANCE_VFT =
        "48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 01 F6 C2 01 74 ?? "
        "BA 40 00 00 00 E8 ?? ?? ?? ?? 90 48 8B C3 48 83 C4 30 5B C3 90 78 ??";
    // WorldSfxMan singleton (was RVA 0x3D6F5F8) — "game world loaded" gate.
    inline constexpr const char *WORLD_SFX_MAN_SLOT =
        "48 8B 05 ?? ?? ?? ?? 48 8D 4D 98 48 89 4C 24 60";

    // ── Render swapchain re-apply (mid-session resolution/mode fix) ──
    // FUN_1419ed440(renderMgr, W, H): the engine's COMPLETE resolution re-apply —
    // release all per-output GPU targets → unconditional ResizeBuffers on every output →
    // refresh source dims from GetDesc → recompute+recreate targets. Fixes windowed /
    // fullscreen / borderless mid-session resize. Call from the Present thread only.
    // (RE: docs/re/windows_midsession_resolution_swapchain_re_followup_findings.md.) The
    // `48 8B 05 <disp>` (loads the render-mgr slot) disp is masked for version-stability.
    inline constexpr const char *RENDER_REAPPLY_RES =
        "40 53 55 56 57 48 81 EC B8 00 00 00 48 8B 05 ?? ?? ?? ?? 48";

    // ── Image RVAs (genuine hardcoded offsets — patch-FRAGILE, re-derive if they move) ──
    // CS::WorldMapCursorControl vtable (doc imagebase 0x140000000).
    inline constexpr uintptr_t CURSOR_VTABLE_RVA = 0x2b29a90;
    // CSMenuMan static slot (Ghidra c843cc3) — world-map cursor = WorldMapDialog + 0x2DB0.
    inline constexpr uintptr_t CSMENUMAN_SLOT_RVA = 0x3d6b7b0;
    // WorldMap canvas singleton (debug-only).
    inline constexpr uintptr_t CANVAS_SINGLETON_RVA = 0x47ef360;
    // CSWorldMapPointMan (native-pin icon manager) — RE: windows_suppress_native_pins_
    // runtime_re_findings.md. Static slot CONFIRMED at the call site (FUN_140623410
    // 0x624296 MOV RCX,[0x143d6e9b0]). The std::map<int id, CSWorldMapPointIns*> _Tree
    // sits at mgr+0x390: _Myhead@+0x398 (ptr), _Mysize@+0x3A0 (count), node key@+0x20 /
    // value@+0x28. Sibling mgr (player-markers/objectives, separate → don't touch).
    inline constexpr uintptr_t ICON_MGR_SLOT_RVA = 0x3d6e9b0;
    inline constexpr uintptr_t ICON_MGR_SIBLING_SLOT_RVA = 0x3d6f558;

    // ── In-game pause (dx-bugs-backlog item 4; technique from iArtorias/elden_pause) ──
    // A `je` in the frame-step path: `je …; mov byte [rbx+XXX],0; lea rsi,[rip+…];
    // mov [rbp-XX],rsi; mov [rbp-XX],edi`. Flipping the je (0F 84) to jne (0F 85) PAUSES the
    // world simulation; restoring resumes it. Byte [1] IS the state (0x84 running / 0x85
    // paused) — readable, so the debug RPC can also report/clear a pause set by another
    // patcher of this same branch (PauseTheGame.dll uses the identical technique).
    // See src/goblin_pause.cpp.
    inline constexpr const char *PAUSE_BRANCH =
        "0F 84 ?? ?? ?? ?? C6 83 ?? ?? 00 00 00 48 8D ?? ?? ?? ?? ?? 48 89 ?? ?? 89";

    // ── FUNC call-targets hardened from fixed RVAs (AOB backlog, 2026-07-04) ──
    // Prologue AOBs crafted from a LIVE mem_dump (packed .text unpacked at runtime) via
    // tools/hf_hook_scout.py disasm --aob; rip-rel disps + rel32 targets wildcarded. A base match
    // IS the function address (no relative_offsets). Each keeps its RVA at the call site as a
    // cross-checked fallback. See docs/re/rva_aob_hardening_backlog.md.
    // FUN_140c70360: PhysWorld cast-ray (heightfield ground probe). RVA 0xc70360.
    inline constexpr const char *CASTRAY_FN =
        "48 8B C4 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 FF FF FF 48 81 EC 90 01 00 00 "
        "48 C7 44 24 30 FE FF FF FF 48 89 58 10";
    // FUN_1406c46e0: geom world-matrix getter (getter(module=inst+0x18, out mat4x4[16])). RVA 0x6c46e0.
    // Extended past the shared prologue (a sibling getter made the short form MULTI) with the distinctive
    // lea rcx,[rbx+0x18] + movss-from-[rax] float loads → unique.
    inline constexpr const char *GEOM_MATRIX_GETTER_FN =
        "48 89 5C 24 08 57 48 81 EC B0 00 00 00 48 8B 01 48 8B D9 0F 29 B4 24 A0 00 00 00 "
        "48 8B FA 48 8B 48 08 E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 48 8D 4B 18 0F 28 1D ?? ?? ?? ?? "
        "48 8D 54 24 60 0F 28 2D ?? ?? ?? ?? F3 0F 10 40 08 F3 0F 10 50 04";
    // FUN_1406a5080: EnsureAssetRequest(reqMgr, wchar_t* name) — geom ADD pivot-2. RVA 0x6a5080.
    inline constexpr const char *ENSURE_ASSET_REQUEST_FN =
        "40 56 57 48 83 EC 58 80 B9 85 01 00 00 00 48 8B F2 48 8B F9 75 ?? 33 C0 48 83 C4 58 5F 5E C3";

    // ── Health check ───────────────────────────────────────────────────────────
    // Scans every AOB above (no relative_offsets — a base match is enough to prove the
    // signature still exists) and logs PASS@addr / FAIL. Call once at init. After an ER
    // update, a FAIL line names exactly which signature to re-find.
    struct SigEntry { const char *name; const char *aob; };

    inline const SigEntry *all_signatures(size_t &count)
    {
        static const SigEntry table[] = {
            {"IS_EVENT_FLAG", IS_EVENT_FLAG},
            {"EVENT_FLAG_MAN_SLOT", EVENT_FLAG_MAN_SLOT},
            {"SET_EVENT_FLAG", SET_EVENT_FLAG},
            {"EVENT_FLAG_MAN_SLOT_ALT", EVENT_FLAG_MAN_SLOT_ALT},
            {"ADD_ITEM_FUNC", ADD_ITEM_FUNC},
            {"INVENTORY_ACCESSOR", INVENTORY_ACCESSOR},
            {"GAME_DATA_MAN", GAME_DATA_MAN},
            {"SAVE_FN", SAVE_FN},
            {"SERIALIZE_FN", SERIALIZE_FN},
            {"LUA_WARP", LUA_WARP},
            {"CS_LUA_EVENT_MANAGER", CS_LUA_EVENT_MANAGER},
            {"SOLO_PARAM_LIST", SOLO_PARAM_LIST},
            {"MSG_REPOSITORY", MSG_REPOSITORY},
            {"GETMESSAGE", GETMESSAGE},
            {"GOODS_TYPE_ACCESS", GOODS_TYPE_ACCESS},
            {"GOODS_SORT_GROUP_ACCESS", GOODS_SORT_GROUP_ACCESS},
            {"AEG_PICKUP_LOT_ACCESS", AEG_PICKUP_LOT_ACCESS},
            {"AEG_REPICK_BIT_ACCESS", AEG_REPICK_BIT_ACCESS},
            {"GEOM_FLAG_SLOT", GEOM_FLAG_SLOT},
            {"WORLD_GEOM_MAN_SLOT", WORLD_GEOM_MAN_SLOT},
            {"GEOM_REQ_MGR", GEOM_REQ_MGR},
            {"GEOM_REQ_MGR_BACKUP", GEOM_REQ_MGR_BACKUP},
            {"WCM_FINDER", WCM_FINDER},
            {"PLAYER_MAPID_SLOT", PLAYER_MAPID_SLOT},
            {"MARKER_CHAIN_SLOT", MARKER_CHAIN_SLOT},
            {"MARKER_ARRAY_CTOR", MARKER_ARRAY_CTOR},
            {"CSMENUMAN_SLOT", CSMENUMAN_SLOT},
            {"WORLDMAP_POINT_FN", WORLDMAP_POINT_FN},
            {"WORLDMAP_POINT_CTOR", WORLDMAP_POINT_CTOR},
            {"WORLDMAP_PROJ_POINT", WORLDMAP_PROJ_POINT},
            {"WORLDMAP_PROJ_LOOP", WORLDMAP_PROJ_LOOP},
            {"WORLDMAP_CONV_BUILD", WORLDMAP_CONV_BUILD},
            {"WORLDMAP_LEGACY_FOLD", WORLDMAP_LEGACY_FOLD},
            {"WORLDMAP_CREATE_IMAGE", WORLDMAP_CREATE_IMAGE},
            {"EC_TEST_DISTANCE_VFT", EC_TEST_DISTANCE_VFT},
            {"WORLD_SFX_MAN_SLOT", WORLD_SFX_MAN_SLOT},
            {"RENDER_REAPPLY_RES", RENDER_REAPPLY_RES},
            {"PAUSE_BRANCH", PAUSE_BRANCH},
            {"CASTRAY_FN", CASTRAY_FN},
            {"GEOM_MATRIX_GETTER_FN", GEOM_MATRIX_GETTER_FN},
            {"ENSURE_ASSET_REQUEST_FN", ENSURE_ASSET_REQUEST_FN},
        };
        count = sizeof(table) / sizeof(table[0]);
        return table;
    }

    // Persistent result of the last resolve_all_signatures() pass. Lets the overlay
    // surface a "signatures unresolved" error to the user instead of only logging it
    // (the log is invisible mid-game). `ran` distinguishes "no failures" from "never ran".
    struct SigHealth { int total = 0, pass = 0, multi = 0, fail = 0; bool ran = false; };
    inline SigHealth &sig_health()
    {
        static SigHealth h;
        return h;
    }

    // Resolve a FUNC by its prologue AOB, cross-checked against its known (build-specific) RVA.
    // Prefers the AOB (patch-portable); falls back to base+rva if the AOB doesn't match (e.g. the
    // code region isn't unpacked yet). Warns once if BOTH resolve but disagree — an AOB drifted to a
    // sibling, or a stale RVA. This is the migration vehicle for docs/re/rva_aob_hardening_backlog.md:
    // a call site passes its old RVA here and gets AOB-first resolution for free.
    inline void *resolve_func_aob(const char *aob, uintptr_t base, uintptr_t rva, const char *name)
    {
        void *by_aob = nullptr;
        try { by_aob = modutils::scan<void>({.aob = aob}); }
        catch (...) { by_aob = nullptr; }
        void *by_rva = base ? reinterpret_cast<void *>(base + rva) : nullptr;
        if (by_aob && by_rva && by_aob != by_rva)
            spdlog::warn("[SIG] {} AOB {} != RVA er+{:#x}={} — using AOB", name, by_aob, rva, by_rva);
        if (!by_aob)
            spdlog::warn("[SIG] {} AOB no match — falling back to RVA er+{:#x}", name, rva);
        return by_aob ? by_aob : by_rva;
    }

    // Logs one line per AOB + a summary. Checks both PRESENCE and UNIQUENESS — a
    // signature that matches >1 site (MULTI) is a latent wrong-function bug: scan()
    // returns only the first match, so it may resolve a sibling instead of the real
    // target (this is exactly how SET_EVENT_FLAG picked a decoy half the time).
    //   FAIL  = 0 matches (gone — re-find after game update)
    //   PASS  = exactly 1 match (unique)
    //   MULTI = >1 match (ambiguous — tighten the AOB until unique)
    // Safe to call once at init.
    inline void resolve_all_signatures()
    {
        size_t n = 0;
        const SigEntry *t = all_signatures(n);
        int pass = 0, multi = 0, fail = 0;
        spdlog::info("[SIG] AOB health check — {} signatures (presence + uniqueness):", n);
        for (size_t i = 0; i < n; ++i)
        {
            size_t cnt = 0;
            void *addr = nullptr;
            try
            {
                cnt = modutils::scan_count(t[i].aob);
                addr = modutils::scan<void>({.aob = t[i].aob});
            }
            catch (...) { cnt = 0; addr = nullptr; }

            if (cnt == 0)
            {
                ++fail;
                spdlog::warn("[SIG]   FAIL  {} (0 matches — re-find after game update)", t[i].name);
            }
            else if (cnt == 1)
            {
                ++pass;
                spdlog::info("[SIG]   PASS  {} -> {}", t[i].name, addr);
            }
            else
            {
                ++multi;
                spdlog::warn("[SIG]   MULTI {} -> {} ({} matches — AMBIGUOUS, may resolve the "
                             "wrong function; tighten the AOB)", t[i].name, addr, cnt);
            }
        }
        spdlog::info("[SIG] {} unique / {} ambiguous / {} missing  (of {}){}",
                     pass, multi, fail, n,
                     (multi || fail) ? "  <-- see warnings above" : "  — all clean");

        SigHealth &h = sig_health();
        h.total = static_cast<int>(n);
        h.pass = pass;
        h.multi = multi;
        h.fail = fail;
        h.ran = true;
    }
}
