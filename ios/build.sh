#!/usr/bin/env bash
# Builds Skate3Recomp.ipa for your own iPhone/iPad from your own game files.
#
#   ios/build.sh [--game <dump dir>] [--tu <TU package>] [--extended-va]
#
# Needs: a Mac with Xcode, Homebrew `llvm cmake ninja`, an extracted dump of
# your Skate 3 disc (default.xex + data/), and ideally Title Update 3
# (TU_12K2276_000000C000000.00000000000O3).
#
# Steps: apply the SDK iOS patch -> run codegen with a host (macOS) build ->
# cross-compile the iOS app -> package build/Skate3Recomp.ipa.
#
# The resulting .ipa contains code recompiled from YOUR copy of the game. Only
# install it on your own devices; do not share or upload it.
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
game="$repo_root/game"
tu="$repo_root/TU_12K2276_000000C000000.00000000000O3"
extended_va=OFF
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game) game="$2"; shift 2 ;;
    --tu) tu="$2"; shift 2 ;;
    --extended-va) extended_va=ON; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ "$(uname)" == Darwin ]] || { echo "iOS builds need macOS with Xcode" >&2; exit 1; }
[[ -f "$game/default.xex" ]] || { echo "no default.xex in $game (extract your disc there or pass --game)" >&2; exit 1; }
tu_args=()
if [[ -f "$tu" ]]; then
  tu_args=(-DSKATE3_TITLE_UPDATE_PACKAGE="$tu")
else
  echo "warning: title update package not found at $tu; building the untested retail path" >&2
fi

cd "$repo_root"
"$repo_root/ios/ensure-submodules.sh"
"$repo_root/ios/apply-sdk-patches.sh"

# 1. Host codegen (macOS). Produces generated/ in the source tree.
cmake --preset macos-release -DSKATE3_GAME_DATA_ROOT="$game" "${tu_args[@]}"
cmake --build --preset macos-release --target generate-all --parallel

# 2. Static MoltenVK for iOS.
moltenvk_args=()
if [[ -z "${VULKAN_SDK:-}" ]]; then
  moltenvk_args=(-DREXGLUE_MOLTENVK_XCFRAMEWORK="$("$repo_root/ios/fetch-moltenvk.sh")")
fi

# 3. Cross-compile and package.
cmake --preset ios-release -DSKATE3_GAME_DATA_ROOT="$game" "${tu_args[@]}" \
  "${moltenvk_args[@]}" -DSKATE3_IOS_EXTENDED_VIRTUAL_ADDRESSING="$extended_va"
cmake --build --preset ios-release --parallel
cmake --build --preset ios-release --target skate3-ipa

echo
echo "Built: $repo_root/out/build/ios-release/Skate3Recomp.ipa"
echo "Install it with SideStore/AltStore or Xcode (Devices window). Personal use only."
