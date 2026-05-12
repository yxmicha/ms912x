#!/usr/bin/env bash
# Enforces Linux kernel commit message conventions for drm/ms912x.
# Receives the path to the commit message file as $1.
set -euo pipefail

msg_file="$1"
subject=$(head -1 "$msg_file")
failed=0

if ! grep -q "^Signed-off-by:" "$msg_file"; then
  echo "error: missing Signed-off-by: line" >&2
  echo "  add one with: git commit -s" >&2
  failed=1
fi

if ! echo "$subject" | grep -qE "^[A-Za-z][A-Za-z0-9/_-]+: ."; then
  echo "error: subject must start with a subsystem prefix" >&2
  echo "  expected format: drm/ms912x: brief description" >&2
  failed=1
fi

exit "$failed"
