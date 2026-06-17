# Backlog

## Royal Capital (m11) disappearance after the Erdtree burns

When the Erdtree burns (`StoryErdtreeOnFire` = flag 118), Leyndell, Royal Capital
becomes the Ashen Capital. The Ashen markers (m35) are now gated to appear only
after the burn (commit gating m35 → eventFlagId 118). The complementary half is not
done: the **Royal Capital markers (m11_05) that become inaccessible in the ashen
state should hide once the burn flag is set.**

Why it's deferred (harder than the Ashen gate):
- It's a *hide-when-flag* (`textDisableFlagId* = 118`), the inverse of the
  show-when-flag gate, and the icon-level hide path is less direct than `eventFlagId`.
- Not every Royal Capital pickup disappears in the ashen state — some persist. Need
  to identify *which* m11_05 markers become unreachable (cross-ref the ashen MSB /
  itemlots) before blanket-hiding, or it would wrongly hide still-valid items.

Scope when picked up: tag the affected m11_05 rows at inject (like `g_ashen_rows`),
then in `apply_map_logic` set their `textDisableFlagId*` / icon-hide to flag 118.

Low priority — the Ashen markers being correctly gated is the important half; the
duplicate Royal markers lingering is a minor cosmetic overlap, not invisible content.

## World-map open freeze (~6 s)

The game re-processes every resident `WorldMapPointParam` row on each map open; cost
is superlinear in row count, so the ~8952 injected rows cost ~6 s. The mod itself is
not the cost (init ~30 ms, no map-open hook). Full diagnosis, bench data, and the
row-count→freeze curve are in [map_open_freeze.md](map_open_freeze.md).

Planned fix: default the densest low-value loot categories off so the resident row
count drops into a sub-~1 s budget (the community/author "general icons" approach),
documenting the perf/coverage trade-off. Region-lazy injection was ruled out (the
overworld map is a single pannable page; see the doc).
