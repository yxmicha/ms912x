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
    \
    `# checkpatch emits FILE_PATH_CHANGES for every new file added to the` \
    `# repo; this is an out-of-tree driver so the paths will never match the` \
    `# upstream kernel tree.` \
    --ignore FILE_PATH_CHANGES \
    \
    `# SPDX_LICENSE_TAG: checkpatch expects the SPDX header on the very first` \
    `# line of each file, but the MacroSilicon source retains the original` \
    `# GPL-2.0 boilerplate comment block instead.  Replacing those headers` \
    `# would alter the copyright attribution.` \
    --ignore SPDX_LICENSE_TAG \
    \
    `# LINUX_VERSION_CODE: checkpatch flags any #if LINUX_VERSION_CODE guard,` \
    `# preferring runtime feature detection.  This driver supports kernels from` \
    `# 4.x through 6.x and some APIs changed incompatibly, so version guards` \
    `# are the only practical compatibility mechanism here.` \
    --ignore LINUX_VERSION_CODE \
    \
    `# CONSTANT_COMPARISON: all yoda-style comparisons in C code have been` \
    `# fixed (constant on right side).  This flag remains suppressed because` \
    `# checkpatch's heuristic also fires on #if LINUX_VERSION_CODE >= ...` \
    `# preprocessor guards, treating LINUX_VERSION_CODE as a "constant" on` \
    `# the left side; these guards cannot be avoided (see LINUX_VERSION_CODE).` \
    --ignore CONSTANT_COMPARISON \
    \
    `# CAMELCASE: the USB subsystem API and many DRM helpers use camelCase` \
    `# names (e.g. usb_device_id, drm_mode_object).  We cannot rename kernel` \
    `# API identifiers.` \
    --ignore CAMELCASE \
    \
    `# DEVICE_ATTR_PERMS and DEVICE_ATTR_FUNCTIONS: checkpatch wants sysfs` \
    `# attributes defined with DEVICE_ATTR_RO/DEVICE_ATTR_RW macros and` \
    `# specific naming conventions.  The vendor sysfs code predates those` \
    `# conventions and restructuring it would change the ABI.` \
    --ignore DEVICE_ATTR_PERMS,DEVICE_ATTR_FUNCTIONS \
    \
    `# EMBEDDED_FILENAME and FSF_MAILING_ADDRESS: the GPL-2.0 license file` \
    `# uses the old FSF postal address and embeds the filename in its header;` \
    `# both are part of the verbatim license text and must not be altered.` \
    --ignore EMBEDDED_FILENAME,FSF_MAILING_ADDRESS \
    || failed=1
done <<< "$commits"

exit "$failed"
