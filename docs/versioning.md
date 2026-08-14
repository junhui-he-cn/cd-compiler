# Versioning and branch policy

The project is currently on the `0.1` release line. The canonical machine-
readable version is stored in `VERSION`; the `0.1.1` release branch carries
`VERSION` value `0.1.1` and the matching annotated Git tag is `v0.1.1`.

## Version rules

Use Semantic Versioning for release tags:

- `MAJOR` changes only when the project reaches a stable `1.0.0` contract and
  later introduces an incompatible public language, CLI, artifact, or VM
  change.
- Before `1.0.0`, increment `MINOR` for a roadmap milestone or an intentional
  user-visible language/tooling contract change. The current baseline is
  `0.1.0`.
- Increment `PATCH` for a compatible bug fix, diagnostic/test correction,
  documentation-only release, or build/reproducibility fix that does not
  change the supported language or artifact contract.

Release tags are immutable. Update `VERSION` and the user-facing version
documentation in the same commit that establishes a release tag. Do not reuse
an existing tag or move a tag after it has been pushed.

## Branch rules

`master` is the integration and release branch. It must remain buildable and
green under the repository verification gate. Start each roadmap slice from
the latest `origin/master` in a focused branch:

The `0.1.1` branch freezes the 0.1.x release line. It is deletion- and
force-push-protected on the remote; compatible maintenance work lands there
through normal fast-forward pushes, while new development continues on
`master`.

```sh
git fetch origin
git switch master
git pull --ff-only origin master
git switch -c feat/<slice-name>
```

Use `feat/` for roadmap work, `fix/` for compatible corrections, `docs/` for
documentation-only work, and `chore/` for maintenance. Keep one independently
reviewable outcome per branch. For simultaneous local work, prefer separate
native Git worktrees so each branch has its own build directory.

Push a branch explicitly with `git push -u origin <branch>`. Rebase a private
branch onto `origin/master` and use `--force-with-lease`; shared branches must
merge the updated base instead. Never force-push `master`.

Before merging a branch, run its focused tests and the full verification gate,
then merge through the repository's normal review/integration path. Delete a
local or remote feature branch only after its merge is confirmed.
