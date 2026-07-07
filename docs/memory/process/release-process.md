# Release process (fork publish template)

Reusable steps to cut a tagged GitHub release of this fork. First applied for **v2.1.0** (2026-07-06,
the first tagged fork release). Claude PREPARES (roll + tag + notes); the **USER pushes + publishes** (repo
rule — Claude never pushes/publishes without an explicit ask).

v2.2.0 (2026-07-07, Windows box) refinements: the release NOTES file is now COMMITTED at the repo root
(`RELEASE_NOTES_vX.Y.Z.md`, user preference) instead of a scratch path. `gh` + git-push auth work on the
Windows box after a one-time `gh auth login` (browser flow) — with the user's explicit go-ahead Claude can
run the push and `gh release create` directly. The DLL asset can come from `build-err/` (same clang-cl
/Brepro toolchain as `build-linux/`); prefer attaching the exact binary that was live-verified. ⚠ cmd.exe
does not understand `\` line continuations — give the user ONE-LINE commands.

## Remotes & versioning (know these first)
- `origin` = **quentin452/ERR-MapForGoblins-DLL** — the fork; **releases go here**.
- `upstream` = VirusAlex/ERR-MapForGoblins-DLL (original MapForGoblins); `gacsam` = Gacsam/Goblin-ERR (other fork).
- **Inherited tags `v1.0.7`…`v2.0.4` exist but are NOT on this master** (from upstream/gacsam, ~2000+ commits
  off-line). A new release version must **avoid those numbers**. The fork's own line started at **v2.1.0**;
  bump minor for a feature batch, patch for fixes, from the last FORK release (not the inherited tags).

## Steps (Claude does 1–3, user does 4–5)
1. **Roll the changelog** (`docs/changelog.md`):
   - Rename `## [Unreleased]` → `## [vX.Y.Z] - YYYY-MM-DD` (keep its intro line, retitle "First tagged…" only
     for the first one).
   - Insert a fresh `## [Unreleased]` above it with `_Nothing yet — the next cycle's entries go here._`.
   - Commit: `docs(changelog): roll [Unreleased] into vX.Y.Z`.
2. **Annotated tag on the roll commit:** `git tag -a vX.Y.Z -m "<one-line highlights>"`. Confirm it's free:
   `git ls-remote --tags origin vX.Y.Z` (empty = free) and `git show vX.Y.Z --stat` points at the roll commit.
3. **Draft release notes** — curate the marquee features (NOT all bullets). Pull titles with:
   `awk '/^## \[vX.Y.Z\]/{f=1} /^## \[/{if(f&&!/vX.Y.Z/)exit} f' docs/changelog.md | grep -E '^- \*\*'`.
   Group into themes; end with "Full changelog: docs/changelog.md [vX.Y.Z]". Save to a scratch `.md`.
4. **USER pushes:** `git push origin master && git push origin vX.Y.Z`.
5. **USER publishes:**
   ```bash
   gh release create vX.Y.Z --repo quentin452/ERR-MapForGoblins-DLL \
     --title "vX.Y.Z — <subtitle>" --notes-file <notes.md> \
     [build-linux/MapForGoblins.dll]   # optional: attach the built DLL as a download asset
   ```
   Note: `MapForGoblins.dll` is the cross-compiled Windows artifact (the shipped single-DLL `build-linux` build).

## Changelog hygiene (already in changelog.md's own workflow header)
- Only log `Fixed`/`Performance` for defects present in **upstream** (or a prior shipped release) — intra-cycle
  churn (introduced AND fixed before release) nets to zero; put its post-mortem in `docs/memory/`, not here.
- Reverted/abandoned work must NOT leave a stale `Added`/`Fixed` line (e.g. the combat-detection line was
  pulled when the vmap switched to the world-freeze).
