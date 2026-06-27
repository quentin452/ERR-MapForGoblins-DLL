# Lot reachability census — am I missing a pass?

**`baked=0` does not prove completeness.** A lot missed by BOTH the bake and the disk passes is invisible to a baked-residual scan. This census rebuilds the oracle from the RAW regulation + MSB + EMEVD, independent of any pass, and asks of every notable lot: *is it reachable, and if not, why?* Regenerate + `git diff` this file to catch coverage regressions (the mirror of `docs/nobake_scoreboard.md`). Source: `tools/lot_reachability_census.py` (ERR profile).

- **Notable universe**: 7421 lots (a real slot-1 item + a one-time obtain flag).
- **Reached**: 5345 (72%) · **Unreached**: 2076.
- The goal is NOT unreached=0 — some lots are legitimately shop-only / quest-give / cut. The goal is **every unreached lot has a named blocker, and none is `missing-pass`**.

## 1. Item-instruction catalog — the missing-pass detector (EMEDF-typed)

Every EMEVD instruction that places/awards an item lot, identified PRECISELY: the lot must sit in an arg the EMEDF types as *Item Lot* (not a coincidental numeric match — that is how a ladder's disable-flag in `2009:00 Register Ladder` was once mistaken for a placement). Only `positions=yes` instructions are markers; `positions=no` ones (`Award Item Lot` etc.) give to the player with no world spot → correctly not markers. **A `positions=yes` instruction the parser does NOT handle is a real missing pass.**

| bank:id | instruction | positions | lots | notable | parsed? |
|---|---|---|--:|--:|---|
| 2003:36 | Award Items (Including Clients) | no (give) | 73 | 67 | n/a (give) |
| 2003:04 | Award Item Lot | no (give) | 69 | 59 | n/a (give) |
| 2004:76 | UNKNOWN 2004[76] | no (give) | 4 | 3 | n/a (give) |

**✅ Every position-bearing item instruction is parsed (or unused) — no direct-instruction missing pass.**

### bank-2000 RunCommonEvent templates carrying notable lots

Templates award via their body (no typed arg here), so this is by invoked template id. A template the parser handles = covered; an unparsed one carrying notable lots with a resolvable anchor is a candidate (but many are treasure-glow/enable templates whose lot is already placed by the MSB Treasure pass — confirm via the per-lot reachability, which marks those `reached:treasure`).

| template | lots | notable | parsed? |
|---|--:|--:|---|
| ev90006900 | 167 | 167 | ⚠️ no |
| ev90005261 | 68 | 68 | ⚠️ no |
| ev90005250 | 58 | 58 | ⚠️ no |
| ev9005810 | 56 | 56 | ⚠️ no |
| ev1100 | 53 | 53 | ⚠️ no |
| ev90005211 | 47 | 47 | ⚠️ no |
| ev90005200 | 45 | 45 | ⚠️ no |
| ev9005800 | 32 | 32 | ⚠️ no |
| ev9005811 | 32 | 32 | ⚠️ no |
| ev9005801 | 31 | 31 | ⚠️ no |
| ev90005201 | 31 | 31 | ⚠️ no |
| ev9005822 | 28 | 28 | ⚠️ no |
| ev90005221 | 20 | 20 | ⚠️ no |
| ev90005251 | 20 | 20 | ⚠️ no |
| ev9300 | 18 | 18 | ⚠️ no |
| ev90005781 | 17 | 17 | ⚠️ no |
| ev90005702 | 15 | 15 | ⚠️ no |
| ev90005870 | 15 | 15 | ⚠️ no |
| ev90005703 | 14 | 14 | ⚠️ no |
| ev90005704 | 14 | 14 | ⚠️ no |
| ev90005744 | 11 | 11 | ⚠️ no |
| ev90005780 | 10 | 10 | ⚠️ no |
| ev90005790 | 10 | 10 | ⚠️ no |
| ev90005872 | 9 | 9 | ⚠️ no |
| ev90005706 | 9 | 9 | ⚠️ no |
| ev90005791 | 9 | 9 | ⚠️ no |
| ev90005882 | 9 | 9 | ⚠️ no |
| ev65850 | 8 | 8 | ⚠️ no |
| ev90005605 | 7 | 7 | ⚠️ no |
| ev11103005 | 7 | 7 | ⚠️ no |
| ev90005511 | 6 | 6 | ⚠️ no |
| ev90005512 | 6 | 6 | ⚠️ no |
| ev9005812 | 6 | 6 | ⚠️ no |
| ev90005646 | 6 | 6 | ⚠️ no |
| ev35003831 | 6 | 6 | ⚠️ no |
| ev90005600 | 6 | 6 | ⚠️ no |
| ev90005620 | 5 | 5 | ⚠️ no |
| ev11002260 | 5 | 5 | ⚠️ no |
| ev18002651 | 5 | 5 | ⚠️ no |
| ev9390 | 4 | 4 | ⚠️ no |

## 2. Reached, by mechanism

| mechanism | lots |
|---|--:|
| treasure | 3544 |
| sibling | 1133 |
| emevd | 542 |
| npc-placed | 126 |
| **total reached** | **5345** |

## 3. Unreached notable lots, by named blocker

`missing-pass` = a position-bearing item instruction we don't parse (FIX). `emevd-template?` = carried by an unparsed bank-2000 template + an anchor — a CANDIDATE to decompile (most are treasure-glow templates referencing an already-placed lot, NOT a gap). `scripted-give` = Award-Item-Lot to the player, no world position (correctly not a marker). `shop-*` = merchant give. `npc-not-placed` = the drop's enemy isn't placed. `orphan-enemy` = `_enemy` row with no NpcParam. `unreferenced` = cut/unused row.

| blocker | count | kind |
|---|--:|---|
| `unreferenced` | 1149 | ✅ likely cut/unused |
| `shop-inf` | 556 | ✅ correct |
| `emevd-template?` | 173 | 🟡 candidate (decompile) |
| `shop-finite` | 77 | ✅ correct |
| `scripted-give` | 72 | ✅ no position |
| `emevd-no-anchor` | 27 | 🟡 RE anchor |
| `orphan-enemy` | 22 | 🟡 investigate |

### Sample unreached per other blocker (first 8)

- **`emevd-no-anchor`** (27): 300(lt1), 2010(lt1), 2020(lt1), 10050(lt1), 10630(lt1), 20120(lt1), 20180(lt1), 20310(lt1)
- **`emevd-template?`** (173): 1054(lt1), 1055(lt1), 1056(lt1), 10000(lt1), 10010(lt1), 20052(lt1), 80120(lt1), 103700(lt1)
- **`orphan-enemy`** (22): 70010000(lt2), 435320006(lt2), 508100700(lt2), 508100710(lt2), 559100702(lt2), 575080701(lt2), 575080702(lt2), 575080711(lt2)
- **`scripted-give`** (72): 2000(lt1), 2230(lt1), 9050(lt1), 20330(lt1), 20340(lt1), 20500(lt1), 30160(lt1), 30640(lt1)
- **`shop-finite`** (77): 1070(lt1), 1130(lt1), 1131(lt1), 1132(lt1), 1133(lt1), 1134(lt1), 1135(lt1), 1136(lt1)
- **`shop-inf`** (556): 1050(lt1), 1051(lt1), 1052(lt1), 1053(lt1), 1057(lt1), 1058(lt1), 1059(lt1), 1060(lt1)
- **`unreferenced`** (1149): 1069(lt1), 2001(lt1), 2520(lt1), 2700(lt1), 5411(lt1), 5412(lt1), 5431(lt1), 10041(lt1)

