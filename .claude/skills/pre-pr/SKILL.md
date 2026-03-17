---
name: pre-pr
description: "Pre-PR pipeline — version bump check, changelog, lint, build, and diff summary"
user-invocable: true
allowed-tools:
  - Read
  - Bash
  - Glob
  - Grep
context:
  - "!cat VERSION 2>/dev/null"
  - "!git diff --stat 2>/dev/null | tail -10"
---

Full pre-PR pipeline

Run the complete spec-draft-simplify-verify pipeline check before opening a PR.

1. Run `make lint` as a fast first gate.
2. Run `make pre-pr` and report results.
3. Check that `VERSION` has been bumped from the previous release.
4. Check that `HISTORY.md` has an entry matching the current `VERSION`.
5. If HTTP API behavior changed, verify `documentation/HTTP_API_CONTRACT.md` is updated.
6. Run `git diff --stat` and summarize what the PR contains.
7. Report any missing items from the PR checklist in `.github/pull_request_template.md`.
