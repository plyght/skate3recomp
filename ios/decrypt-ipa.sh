#!/usr/bin/env bash
# Decrypts the .ipa produced by the "iOS app" GitHub workflow.
#
#   ios/decrypt-ipa.sh Skate3Recomp.ipa.enc        # writes Skate3Recomp.ipa
#
# Uses the same password as SKATE3_FILES_PASSWORD. Works with the openssl that
# ships with macOS and with OpenSSL 3 on Linux/Windows (Git Bash).
set -euo pipefail
in="${1:?usage: ios/decrypt-ipa.sh Skate3Recomp.ipa.enc}"
out="${2:-${in%.enc}}"
if [[ -z "${SKATE3_FILES_PASSWORD:-}" ]]; then
  read -r -s -p "Password: " SKATE3_FILES_PASSWORD
  echo
  export SKATE3_FILES_PASSWORD
fi
openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 \
  -pass env:SKATE3_FILES_PASSWORD -in "$in" -out "$out"
echo "Wrote $out - install it with SideStore, AltStore or Xcode. Do not share it."
