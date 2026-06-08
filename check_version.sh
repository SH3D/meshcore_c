#!/usr/bin/env bash
#
# check_version.sh -- verify the library version is consistent and released.
#
# A read-only sanity check (it changes nothing). It confirms the version is the
# same in all three places it's declared:
#
#   * library.properties              version=X.Y.Z      (Arduino Library Manager)
#   * library.json                    "version": "X.Y.Z" (PlatformIO Registry)
#   * src/meshcore_companion.h        MESHCORE_COMPANION_VERSION "X.Y.Z"
#
# ...and that a matching git tag (vX.Y.Z or X.Y.Z) exists. Run it before tagging
# a release. Exit status is non-zero if anything is inconsistent or untagged.
#
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")" || exit 2

ok()   { printf '  \033[32mok\033[0m   %s\n' "$*"; }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
note() { printf '       %s\n' "$*"; }

prop_ver=$(sed -nE 's/^version=([^[:space:]]+).*/\1/p' library.properties | tr -d '\r' | head -1)
json_ver=$(sed -nE 's/.*"version"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/p' library.json | head -1)
hdr_ver=$(sed -nE 's/.*MESHCORE_COMPANION_VERSION[[:space:]]+"([^"]+)".*/\1/p' src/meshcore_companion.h | head -1)

printf 'Declared versions:\n'
printf '  library.properties        : %s\n' "${prop_ver:-<not found>}"
printf '  library.json              : %s\n' "${json_ver:-<not found>}"
printf '  meshcore_companion.h      : %s\n' "${hdr_ver:-<not found>}"
printf '\nChecks:\n'

fail=0

# 1) all three present and identical
if [ -z "$prop_ver" ] || [ -z "$json_ver" ] || [ -z "$hdr_ver" ]; then
    bad "could not read a version from every source"
    fail=1
elif [ "$prop_ver" = "$json_ver" ] && [ "$json_ver" = "$hdr_ver" ]; then
    ok "all three versions match ($prop_ver)"
else
    bad "versions disagree (properties=$prop_ver json=$json_ver header=$hdr_ver)"
    fail=1
fi

ver="$prop_ver"

# 2) a matching git tag exists (vX.Y.Z or X.Y.Z)
if ! git rev-parse --git-dir >/dev/null 2>&1; then
    bad "not a git repository -- cannot check for a release tag"
    fail=1
elif [ -z "$ver" ]; then
    bad "no version to match a tag against"
    fail=1
else
    tag=""
    for cand in "v$ver" "$ver"; do
        if git rev-parse -q --verify "refs/tags/$cand" >/dev/null 2>&1; then tag="$cand"; break; fi
    done
    if [ -n "$tag" ]; then
        ok "git tag '$tag' exists"
        if [ "$(git rev-list -n1 "$tag")" != "$(git rev-parse HEAD)" ]; then
            note "(tag '$tag' is not on the current HEAD -- fine if you've moved on since the release)"
        fi
    else
        bad "no git tag 'v$ver' or '$ver' -- create one to release: git tag v$ver"
        fail=1
    fi
fi

printf '\n'
if [ "$fail" -eq 0 ]; then
    printf '\033[32mVersion %s is consistent and tagged.\033[0m\n' "$ver"
else
    printf '\033[31mVersion check failed.\033[0m\n'
fi
exit "$fail"
