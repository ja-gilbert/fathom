# Contributing

Working conventions for fathom. It's a solo project, but these rules keep the history readable and make every change traceable to an issue.

## Branches

- Branch off `main` for every change; never commit to `main` directly.
- Name branches `feat/<issue#>-short-slug`, e.g. `feat/4-build-book`. Use `fix/`, `perf/` or `docs/` instead of `feat/` when that describes the work better.
- One branch and one pull request per issue.

## Pull requests

- Open the PR against `main` and fill in the template, including `Closes #N` so the issue closes on merge.
- It must build clean with `-Wall -Wextra -Wpedantic` and pass the tests.
- **Squash merge.** The PR title becomes the commit message on `main`, so write it like a commit subject.
- Delete the branch after merging.

## Commit messages

- Imperative mood: "Add ITCH message counter", not "Added" or "Adds".
- Subject line under 72 characters, no trailing period.
- Optional body, wrapped at 72 characters, that explains *why*, not what.

## Issues

- Use the **Task** template for planned work and the **Bug** template for defects.
- Give each issue one `type:` label and one `area:` label. `priority:p1` means it blocks the next milestone; `priority:p2` is everything else.
- Fill in `Blocked by #N` so the order of work is explicit.
