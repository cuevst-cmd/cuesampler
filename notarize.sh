#!/bin/bash
# Notarize CUE SAMPLER (VST3 + AU) for distribution on macOS.
#
# Prerequisites (one time):
#   1. Developer ID Application cert in keychain (already installed).
#   2. Notary credentials stored once:
#        xcrun notarytool store-credentials "cue-notary" \
#          --key ~/private_keys/AuthKey_XXXX.p8 --key-id XXXX --issuer <uuid>
#
# Usage:  ./notarize.sh
set -euo pipefail

IDENTITY="Developer ID Application: JERRY OTTAVIO VOLPE (KUU9K5SWA8)"
PROFILE="cue-notary"

VST3="build/CueSampler_artefacts/Release/VST3/CUE SAMPLER.vst3"
AU="build/CueSampler_artefacts/Release/AU/CUE SAMPLER.component"

sign_bundle () {
  local bundle="$1"
  echo "==> Signing embedded libraries in: $bundle"
  # Sign nested dylibs / frameworks FIRST (inside-out).
  find "$bundle" \( -name "*.dylib" -o -name "*.so" \) -print0 |
    while IFS= read -r -d '' lib; do
      echo "    - $lib"
      codesign --force --timestamp --options runtime \
        --sign "$IDENTITY" "$lib"
    done

  echo "==> Signing bundle: $bundle"
  codesign --force --timestamp --options runtime \
    --sign "$IDENTITY" "$bundle"

  echo "==> Verifying signature"
  codesign --verify --deep --strict --verbose=2 "$bundle"
}

notarize_bundle () {
  local bundle="$1"
  local zip="${bundle##*/}.zip"
  echo "==> Zipping for submission: $zip"
  ditto -c -k --keepParent "$bundle" "/tmp/$zip"

  echo "==> Submitting to Apple (this waits for the result)"
  xcrun notarytool submit "/tmp/$zip" \
    --keychain-profile "$PROFILE" --wait

  echo "==> Stapling ticket to: $bundle"
  xcrun stapler staple "$bundle"
  xcrun stapler validate "$bundle"
  rm -f "/tmp/$zip"
}

for b in "$VST3" "$AU"; do
  [ -e "$b" ] || { echo "MISSING: $b"; exit 1; }
  sign_bundle "$b"
  notarize_bundle "$b"
done

echo
echo "All done. Final Gatekeeper assessment:"
spctl -a -vvv -t install "$VST3" 2>&1 || true
echo "CUE SAMPLER is signed, notarized, and stapled."
