---
name: release
description: >-
  Cut a new tagged MapForGoblins release to GitHub (quentin452/ERR-MapForGoblins-DLL): finalize the
  changelog, build the shipped single-DLL, push master + a semver tag, and publish a gh release with
  curated notes and the MapForGoblins.dll asset. Use when the user says "release", "sortons la mise à
  jour", "cut a version", "publish to github", "new release", or names a version to ship.
---

# Release a new MapForGoblins version

Publishes a versioned GitHub release. This is **outward-facing and hard to undo** — confirm the version
number and title with the user before the `gh release create` step (the tag + release are public once
pushed). Prior releases: v2.1.0 (first fork release), v2.2.0, v2.3.0, v2.4.0.

## Conventions (from the existing releases — match them)

- **Target repo / remote:** `origin` = `quentin452/ERR-MapForGoblins-DLL`. NOT `gacsam` (Gacsam/Goblin-ERR)
  or `upstream` (VirusAlex). `gh` is authed as `quentin452`.
- **Tag:** `vX.Y.Z` (annotated). Minor bump (`v2.3.0`→`v2.4.0`) when the cycle adds user-facing features;
  patch bump (`vX.Y.Z+1`) for a fixes-only cycle.
- **Release title:** `vX.Y.Z — <short theme>` (em dash). Theme = the 1–3 biggest user-facing items.
- **Asset:** attach the shipped **single-DLL** `build-linux/MapForGoblins.dll` (every prior release ships it).
- **Notes:** curated, user-facing markdown (see v2.3.0 / v2.4.0 for the house style — `## ★ headline`,
  short prose, an Install line). Derive from the changelog's version block, NOT raw git log.

## Steps

1. **Determine scope.** `git log --oneline <lastTag>..HEAD` and read `docs/changelog.md`'s `[Unreleased]`.
   Decide the version bump (feature → minor, fixes-only → patch). Confirm version + title with the user
   (use AskUserQuestion; recommend the semver-correct bump).

2. **Finalize the changelog.** The `[Unreleased]` block is the release body-in-progress. Add any
   user-facing change from `<lastTag>..HEAD` that is missing — but follow the changelog discipline:
   only log a `Fixed`/`Performance` entry for a defect that was **present in the last shipped tag** (a
   migrating user would perceive it); a bug introduced *and* fixed within this cycle is intra-cycle and
   goes to `docs/memory/`, not here. Then rename the block:
   `## [Unreleased]` → leave empty, insert `## [vX.Y.Z] - YYYY-MM-DD` below it. Commit:
   `docs(changelog): cut vX.Y.Z — <theme>`.

3. **Build the shipped DLL fresh from HEAD:** `ninja -C build-linux MapForGoblins` (must be green).
   (If the change crossed the host↔render boundary this cycle, also confirm the split still links:
   `ninja -C build-linux-hotreload MapForGoblins goblin_overlay_render`.)

4. **Push master:** `git fetch origin`; `git push origin master`. (The user normally pushes, but a
   release explicitly authorizes it.)

5. **Tag + push tag:** `git tag -a vX.Y.Z -m "vX.Y.Z — <theme>"`; `git push origin vX.Y.Z`.

6. **Write notes** to a scratchpad file (house style — headline the marquee features, then a Stability/
   quality section, then an Install line), then publish:
   ```
   gh release create vX.Y.Z --repo quentin452/ERR-MapForGoblins-DLL \
     --title "vX.Y.Z — <theme>" --notes-file <notes.md> \
     build-linux/MapForGoblins.dll
   ```

7. **Verify:** `gh release view vX.Y.Z --repo quentin452/ERR-MapForGoblins-DLL` — confirm
   `draft:false`, the tag, and `asset: MapForGoblins.dll`. Report the release URL to the user.

## Notes

- Do NOT touch the `gacsam`/`upstream` remotes.
- If a boot device-reset or other regression is suspected in the shipped DLL, flag it to the user before
  publishing — a release is public immediately (not a draft).
