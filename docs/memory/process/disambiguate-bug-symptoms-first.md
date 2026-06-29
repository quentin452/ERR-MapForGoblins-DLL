---
name: disambiguate-bug-symptoms-first
description: Ask sharp clarifying questions to pin an ambiguous bug symptom BEFORE coding/building/dialing — ambiguity has burned many build-test cycles
metadata: 
  node_type: memory
  type: feedback
---

When the user reports a vague symptom (e.g. "décalage" / "drift" / "lag" / "it's off"), STOP and ask 1–3 sharp disambiguating questions BEFORE writing code, building, deploying, or starting a dial/capture loop. Guessing the wrong interpretation has already cost many wasted build→deploy→test cycles this project.

**Why:** the overlay marker "décalage" bug was one word covering several distinct bugs. I burned hours building wrong fixes (1-frame lead/extrapolation, screen scale, resolution ratio, convScale on the world→render converter, snap-rect viewport map) because I never pinned the symptom first. Each wrong guess = a full clang-cl build + deploy + the user re-testing in-game. The real distinctions only came out after the user got frustrated: static-vs-dynamic, absolute-position-vs-scroll-transition, which axis, error-grows-with-zoom-or-distance, direction relative to motion, settles-at-rest-or-permanent.

**How to apply:** for any ambiguous visual/behavioral bug, ask up front (use AskUserQuestion or a tight prose list) to nail:
- WHEN: static/at-rest vs only-during-motion (scroll/zoom) vs only-on-transition.
- WHAT moves: absolute position (where it sits when still) vs the dynamic transition.
- DIRECTION: which way relative to the input (trails / leads / opposite).
- SCALING: constant vs grows-with-distance-from-centre vs grows-with-zoom/speed.
- RECOVERY: settles when motion stops, or stays wrong permanently.
- WHICH input drives it (mouse/cursor vs pan/scroll vs zoom vs gamepad).

One round of these questions is far cheaper than one wrong build cycle. Prefer asking over a plausible-but-unverified fix, ESPECIALLY before committing to a multi-iteration in-game dial/capture loop. Related: [[overlay-rendered-markers]].
