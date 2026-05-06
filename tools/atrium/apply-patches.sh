#!/bin/sh
# Re-apply Atrium's minimum upstream-file patches after a merge from
# upstream/main. Runs from the repo root. Idempotent — patches that
# already apply forward are applied; patches whose every hunk is already
# present are skipped; mixed states are an error (upstream churn collided
# with one of our hunks).
#
# Use `patch --forward` to refuse auto-reverse: if some-but-not-all hunks
# are already present, we want to know, not silently undo our changes.
set -e
cd "$(dirname "$0")/../.."
for p in .atrium-patches/*.patch; do
  if patch --forward -p1 -N --dry-run -s -i "$p" >/dev/null 2>&1; then
    echo "applying $p"
    patch --forward -p1 -N -i "$p"
    continue
  fi
  if patch -R -p1 --dry-run -s -i "$p" >/dev/null 2>&1; then
    echo "patch already applied: $p (skipping)"
    continue
  fi
  echo "patch in mixed state (some hunks applied, some not): $p" >&2
  echo "resolve by hand — likely upstream churn in the touched files." >&2
  exit 1
done
