# ADR: Content-aware CI / merge gate

Date: 2026-11-23
Status: Accepted
Deciders: Antoine Aflalo (Belphemur), Hermes

## Context

The repo now has two fundamentally different change surfaces:

1. **Firmware** — ESP32 C/C++ code, PlatformIO builds for 8 boards,
   clang-format, cppcheck, host unit tests.
2. **Website** — the VitePress documentation site under `website/`, built
   with bun, validated by a production build (`bunx --bun vitepress build`).

Previously every PR ran the full firmware matrix regardless of what it
touched: a docs-only change burned ~8 Board builds for nothing, and the
website had **no automated build gate at all** — a broken site could merge
undetected (the site's /flash page pins release assets at build time, so a
build failure is user-visible).

Additionally, external bot checks (Kody, CodeRabbit) are advisory and can
fail for infra reasons (e.g. reviewer API credits), which must never block
merging — the real blocker must be our own CI.

## Decision

The existing **`CI Status`** job stays the single required status check
(unchanged name → no branch-protection reconfiguration), but `ci.yml`
becomes content-aware:

1. A `changes` job classifies the PR diff against the merge base:
   - `website` — the PR touches `website/**`
   - `firmware` — the PR touches **anything else** (conservative default:
     `.github/**`, root files, source — all count as firmware).
   On non-PR triggers (push to develop/master, dispatch) both are `true`.

2. Firmware jobs (`build` matrix, `clang-format`, `cppcheck`,
   `unit-tests`) run only when `firmware == true`; otherwise they are
   **skipped** (not failed).

3. A new `website` job runs `bun install --frozen-lockfile` +
   `bunx --bun vitepress build` + output sanity checks
   (`index.html`, `flash.html`, `changelog.html` present) when
   `website == true`.

4. The `CI Status` aggregator depends on all of them with `if: always()`
   and fails only on `failure`/`cancelled` results — skipped jobs pass
   through, so website-only PRs get a green gate from the site build alone.

## Consequences

- Website-only PRs: the 8-board matrix is skipped entirely — minutes of
  runner time saved per docs PR.
- Firmware-only PRs: no bun/site overhead.
- Mixed PRs (e.g. this one, which touches both `website/**` and the
  workflow): both gates run.
- The website can no longer merge broken: the production build is the gate,
  matching the local `bunx --bun vitepress build` verification.
- Advisory bot checks remain outside the required set; `mergeability` is
  decided by `CI Status` alone.
- Deployment (Workers Builds → xerif.aaflalo.me) remains dashboard-side and
  is documented in `website/README.md`; CI validates but does not deploy.

## Rejected alternatives

- **Third-party path-filter action** (e.g. `dorny/paths-filter`): avoids a
  dependency; the classification is 10 lines of shell.
- **Separate website workflow with `paths:` trigger**: GitHub creates no
  check run when a path-filtered workflow is skipped, which would leave a
  required check pending forever on website-only PRs.
- **A separate `merge-gate.yml`**: duplicates the aggregator pattern and
  forces a branch-protection change for zero benefit over extending the
  existing one.
