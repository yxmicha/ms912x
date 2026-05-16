#!/usr/bin/env bash
# Runs checkpatch.pl on every commit not yet on the remote tracking branch.
set -euo pipefail

if ! command -v checkpatch.pl &>/dev/null; then
  echo "error: checkpatch.pl not found in PATH — enter the nix devShell first" >&2
  exit 1
fi

upstream=$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)
if [ -n "$upstream" ]; then
  range="${upstream}..HEAD"
else
  range="HEAD"
fi

commits=$(git rev-list "$range") || true
if [ -z "$commits" ]; then
  exit 0
fi

failed=0

while IFS= read -r commit; do
  echo "--- checking $commit ---"
  checkpatch.pl \
    --git "$commit" \
    --no-tree \
    --strict \
    --ignore FILE_PATH_CHANGES,SPDX_LICENSE_TAG \
    --ignore LINUX_VERSION_CODE,CONSTANT_COMPARISON \
    --ignore CAMELCASE \
    || failed=1
done <<< "$commits"

exit "$failed"
