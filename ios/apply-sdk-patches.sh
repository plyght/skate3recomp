#!/usr/bin/env bash
# Applies patches/rexglue-sdk-ios.patch to third_party/rexglue-sdk. Safe to
# rerun: an already-applied patch is detected and skipped.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="$repo_root/third_party/rexglue-sdk"
patch="$repo_root/patches/rexglue-sdk-ios.patch"

if git -C "$sdk" apply --reverse --check "$patch" 2>/dev/null; then
  echo "rexglue SDK iOS patch already applied"
  exit 0
fi
git -C "$sdk" apply --whitespace=nowarn "$patch"
echo "Applied rexglue SDK iOS patch"
