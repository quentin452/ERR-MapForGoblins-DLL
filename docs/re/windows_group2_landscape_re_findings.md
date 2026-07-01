# Findings — MapGenie Group 2 landscape (which non-WMPP categories are wire-able, and how)

After Portal shipped (`AEG099_510` × EMEVD warp template `90005605` — see
`windows_portal_aeg_re_findings.md`), this pass reconnoitred the REST of Group 2 to see which follow the
same clean recipe (find AEG model → find its harvestable EMEVD template → one pass). **Verdict: Portal
was the clean one. The remaining Group-2 categories do NOT have a harvestable EMEVD-template signal —
they need heavier per-category RE.** Tools: `tools/_probe_g2_templates.py` (template→bound-model map),
`_probe_g2_actionbtn.py` (ActionButtonParam→model/template link).

## Method + what's reusable

- **Template→model recon** (`_probe_g2_templates.py`): for every shared common template (90005xxx/
  90006xxx), resolve its bank-2000 call args to the MSB models they bind, tally template→{model}. High
  "purity" (one dominant model) = a wire-able feature. This map is committed/reusable.
- **ActionButtonText anchors** (extracted from `menu_dlc02.msgbnd` `ActionButtonText.fmg`): the interaction
  prompt names the feature. Key ids found:
  - **Smithing Table** = ActionButtonText **7030** "Use smithing table" → ActionButtonParam **6250**.
  - **Elevator** = ActionButtonText **3301** "Descend" → ActionButtonParam **5010**; also **3000**
    "Pull lever" → ABP {8200,8300–8420,…} (levers, broader than lifts).
  - **Hidden Passage**: **no** ActionButtonText — illusory walls are HIT-detected (you attack them),
    not interacted → no action-button/asset-interaction signal at all. Hardest.

## Why the rest don't fit the Portal recipe

- **Elevator / Smithing Table are ObjAct-bound, not EMEVD-template-bound.** `_probe_g2_actionbtn.py`:
  ActionButtonParam 5010 (Descend) co-occurs with only 2 EMEVD entities (noise: `c7520`, `AEG463_650`);
  ABP 6250 (smithing) co-occurs with 0. So the asset→action-button binding is NOT carried in EMEVD args
  — it lives in **ObjAct / AssetObjActParam** (the asset's own object-action table), a different RE path.
- **No candidate AEG model cleanly matches the MapGenie counts.** The twin templates `90005950`/`90005951`
  (100%-pure `AEG099_630`, ~28 bound entities) looked like an up/down lift pair, but `AEG099_630` has
  **235 broad placements** across every area — a generic asset, not the ~40 elevators. `AEG460_080`
  (115, all DLC area 61), `AEG099_205-208`, `AEG099_990` (312) are likewise broad/DLC-specific, none = a
  named Group-2 category. Wiring any of these on the structural signal alone would draw the wrong set.

## Recommendation — remaining Group 2 is an ObjAct-param investigation, not a quick win

To wire Elevator / Smithing Table cleanly, the next RE step is **ObjActParam / AssetObjActParam**: find
the rows whose action-button = 5010 (Descend) / 6250 (smithing), get their ObjAct id, then find which MSB
assets carry that ObjAct (MSB Asset ObjAct fields) → the real asset set. That's the analogue of the
`90005605` harvest but on the ObjAct param instead of EMEVD. Hidden Passage (hit-based) and Wandering
Mausoleum (dynamic entity) have no static interactable signal and are separate, harder problems.

Scope note: this is materially more work than Portal (a new param path + MSB ObjAct-field parse per
category), so it should be picked up deliberately, not as a quick follow-on. Recon artifacts
(`_probe_g2_templates.py`, `_probe_g2_actionbtn.py`) + the ABP ids above are the starting point.
Environment caveat: offline SoulsFormats probes on this box currently need temp files written to the
**repo dir** (`os.path.abspath('.')`), not `%TEMP%` — Windows Defender real-time protection began denying
`%TEMP%`/scratchpad writes mid-session (`WinError 5`); the repo-dir temp path works.
