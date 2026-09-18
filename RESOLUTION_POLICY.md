# Sunrise merge resolution policy (0.5.0 merge)

Repo root: `C:\Users\Seth\Sunrise`   (git worktree)
Source root prefix for all paths below: `Sunrise/`

## Situation
We are mid-`git merge origin/master` on branch `feature/gear-editor`. The merge
brought upstream 0.5.0 (a large refactor, +15110/-8131) into a fork that carries
co-op / Gear Editor / spawner / Cowisma features. HEAD side = fork (co-op features,
older 0.4.0-era code). Currently-merging origin/master side = 0.5.0 (new architecture).

The fork's own reference tip that matches this work is a git remote:
   `pr-Techno453-Sunrise-coop-fork/coop-shared-exploration`
which ONLY contains the co-op feature set (NOT 0.5.0). Use it as the authority for
what the co-op feature code must look like, but ALWAYS combine it with origin's 0.5.0
architecture.

## Goal of each resolution
Produce a single source file that:
1. KEEPS all fork co-op / gear-editor functionality (these features are the whole
   point of the fork; do NOT drop them).
2. ADOPTS upstream 0.5.0's refactors/renames/rewrites wherever they do not conflict
   with the co-op feature, and wherever 0.5.0 changed the same code.
3. Compiles.

## Rules (in priority order)
1. NEVER delete a fork feature to make the conflict disappear. If origin/0.5.0
   refactored a region that the fork's co-op code also touches, keep the fork's
   behavior and translate it onto the 0.5.0 structure.
2. When both sides added code that does not collide (different helper functions,
   different detour entries, additive members), KEEP BOTH.
3. When 0.5.0 renamed a symbol (e.g. target/body naming, namespace moves, settings
   renamed), PREFER the 0.5.0 name and update the fork's references to it.
4. When 0.5.0 retired a subsystem that the co-op fork DID NOT need to keep
   (e.g. legacy sign-on-readiness hooks, bootflow hooks, deprecated settings),
   follow 0.5.0 and drop the fork's references to it. This matches the direction
   the co-op fork authors themselves took.
5. Keep code style consistent: 79-col, same include order as neighbors, same
   naming conventions as the surrounding file.
6. Do not leave any `<<<<<<<`, `=======`, `>>>>>>>` markers. Do not leave duplicated
   namespaces or half-classes. The file must read as clean, intentional code.
7. Before finishing a file: confirm there are no marker lines, and re-read the
   touched regions to sanity-check that both the feature and the 0.5.0 structure
   are intact.

## Reference commands (run in C:\Users\Seth\Sunrise)
- Show the co-op-fork-tip version of a file (the feature-authoritative view):
      git show pr-Techno453-Sunrise-coop-fork/coop-shared-exploration:<path>
- Show origin 0.5.0 version of a file:
      git show origin/master:<path>
- Show the fork (HEAD) version:
      git show HEAD:<path>

## Workflow per file
- Read the conflicted file fully around the markers.
- Read origin/master's version and, where useful, the co-op-fork tip version of the
  same file to decide.
- Edit the work-tree file to the resolved form. (Use the Edit tool with exact
  match strings from the file content; read first.)
- Verify: no marker lines remain; spot-check the merged logic.
- DO NOT `git add` unless told. DO NOT commit. Report per-file what you did.

## Report back (final message)
For EACH file you handled, give a one-line summary: file path -> what HEAD had,
what origin had, what you kept, and any doubts. Also list any file where you are
not fully confident so the coordinator can re-check.
