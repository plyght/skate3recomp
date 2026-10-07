#!/usr/bin/env bash
# Downloads a MoltenVK release and prints the static MoltenVK.xcframework path.
# Skip this if you already have the LunarG Vulkan SDK (set VULKAN_SDK instead).
set -euo pipefail
version="${MOLTENVK_VERSION:-v1.3.0}"
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
dest="$repo_root/out/moltenvk-$version"
xcf="$dest/MoltenVK/MoltenVK/static/MoltenVK.xcframework"

if [[ ! -f "$xcf/ios-arm64/libMoltenVK.a" ]]; then
  mkdir -p "$dest"
  url="https://github.com/KhronosGroup/MoltenVK/releases/download/$version/MoltenVK-ios.tar"
  echo "Downloading $url" >&2
  curl -fL --retry 3 -o "$dest/MoltenVK-ios.tar" "$url"
  tar -xf "$dest/MoltenVK-ios.tar" -C "$dest"
fi
if [[ ! -f "$xcf/ios-arm64/libMoltenVK.a" ]]; then
  found="$(find "$dest" -path '*static/MoltenVK.xcframework' -type d | head -n1)"
  [[ -n "$found" ]] || { echo "static MoltenVK.xcframework not found in $dest" >&2; exit 1; }
  xcf="$found"
fi
echo "$xcf"
