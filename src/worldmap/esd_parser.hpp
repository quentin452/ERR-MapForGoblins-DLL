#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// ESD (EzState, magic fsSL/fSSL) talk-script parser — the runtime C++ port of the
// structured reader (SoulsFormats ESD.cs, merchant_item_search_plan.md option A).
// Scope: READ-only enumeration of CommandCalls + decoding of the literal
// `82 <int32 LE> A1` EzState argument form (78% of all args, and 100% of the ones
// we consume — see docs/re/esd_ezstate_decoder_re_findings.md). It does NOT
// evaluate branching EzState expressions (the 21%) and does NOT write ESD.
//
// Layout (from SoulsFormats ESD.cs, oracle-validated vs tools/esd_shop):
//   header 0x6c bytes (26 i32 after the magic) → dataStart
//   dataStart: 5 i32 (+1 pad i32 in long format), then 6 varints
//     {stateGroupsOffset, stateGroupCount, nameOffset, nameLength, unk, unk}
//   state-group table: count × {id, statesOffset, stateCount, statesOffset} varints
//   states:     stateCount × 9 varints {id, condsOff, condsCnt, entryOff, entryCnt,
//                                       exitOff, exitCnt, whileOff, whileCnt}
//   conditions: condCount × 7 varints {stateOff, passOff, passCnt, condsOff,
//                                      condsCnt, evalOff, evalLen}
//   command call = {bank i32, id i32, argsOff varint, argsCnt varint}; the args
//   table is argsCnt × {argOff varint, argSize varint}; every offset is relative
//   to dataStart. States + conditions are stored SEQUENTIALLY (the C# reader
//   depends on that too), so walking them enumerates every command list.
// "varint" = i64 in long format (fsSL — Elden Ring talk), i32 in short (fSSL).
namespace goblin::esd
{
// One `OpenRegularShop(shopBegin, shopEnd)` (talk command bank 1 id 22) occurrence:
// the owning t<TalkID>.esd sells ShopLineupParam rows [begin, end].
struct TalkShopRange
{
    uint32_t talkId = 0;
    int32_t  begin = 0, end = 0;
};

// Enumerate every CommandCall of `bank`:`id` in ONE decompressed ESD blob and
// return each call whose two arguments are both literal `82 <i32> A1` expressions
// as an (arg0, arg1) pair. Calls with a different arg count / non-literal args are
// counted into *skippedExpr (when non-null) and dropped — for 1:22 the RE showed
// every real instance is a clean literal pair. Empty on any malformed blob.
std::vector<std::pair<int32_t, int32_t>> collect_literal_pairs(const uint8_t *buf, size_t len,
                                                               int bank, int id,
                                                               int *skippedExpr = nullptr);

// Walk a DECOMPRESSED talkesdbnd BND4 and collect every member t<TalkID>.esd's
// OpenRegularShop (1:22) ranges. Non-ESD members and non-t-named entries are
// skipped. Empty on a malformed BND4.
std::vector<TalkShopRange> parse_talkbnd_shop_ranges(const uint8_t *buf, size_t len,
                                                     int *skippedExpr = nullptr);
} // namespace goblin::esd
