#!/bin/sh
# Re-apply Atrium's minimum upstream-file patches after a merge from
# upstream/main. Runs from the repo root. Idempotent — patches that
# already apply silently skip.
set -e
cd "$(dirname "$0")/../.."
for p in .atrium-patches/*.patch; do
  if patch -R -p1 --dry-run -s -i "$p" >/dev/null 2>&1; then
    echo "patch already applied: $p (skipping)"
    continue
  fi
  echo "applying $p"
  patch -p1 -i "$p"
done
