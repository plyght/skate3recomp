#!/usr/bin/env bash
# Initializes the SDK's submodules. The SDK pins Dear ImGui to a commit that is
# no longer fetchable from github.com/ocornut/imgui ("not our ref"), which makes
# a plain `git submodule update --init --recursive` fail. Every other submodule
# is initialized normally; ImGui falls back to its docking branch only when the
# pinned commit cannot be fetched (an existing working checkout is kept).
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="$repo_root/third_party/rexglue-sdk"

git -C "$repo_root" submodule update --init third_party/rexglue-sdk
# (macOS ships bash 3.2: no mapfile.)
git -C "$sdk" config --file .gitmodules --get-regexp '\.path$' | awk '{print $2}' |
  while read -r path; do
    if [[ "$path" == thirdparty/imgui ]]; then
      continue
    fi
    git -C "$sdk" submodule update --init --recursive --depth 1 "$path"
  done

imgui="$sdk/thirdparty/imgui"
if [[ -f "$imgui/imgui.h" ]]; then
  exit 0
fi
if git -C "$sdk" submodule update --init thirdparty/imgui 2>/dev/null; then
  exit 0
fi
echo "warning: the SDK's pinned ImGui commit is unavailable upstream; using the docking branch" >&2
rm -rf "$imgui"
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git "$imgui"
