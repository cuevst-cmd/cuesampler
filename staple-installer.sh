#!/bin/bash
# Run in your own Terminal:  cd ~/Documents/SAMPLERv3 && ./staple-installer.sh
# Waits for the already-submitted VST3 installer to clear Apple, then staples it.
set -uo pipefail
PROFILE="cue-notary"
PKG="dist/CUE SAMPLER 1.0.0.pkg"
SUB="ecd4e4e3-1b7c-45f9-a057-3aacb66ad50d"

echo "Waiting on Apple for the installer (every 60s)..."
while true; do
  S=$(xcrun notarytool info "$SUB" --keychain-profile "$PROFILE" 2>/dev/null \
        | awk '/status:/{print $2; exit}')
  echo "$(date '+%H:%M:%S')  installer: ${S:-unknown}"
  case "$S" in
    Accepted) break ;;
    Invalid|Rejected)
      echo "FAILED. Log:"; xcrun notarytool log "$SUB" --keychain-profile "$PROFILE"; exit 1 ;;
  esac
  sleep 60
done

echo "==> Accepted. Stapling..."
xcrun stapler staple "$PKG" && xcrun stapler validate "$PKG"
echo "==> Gatekeeper check:"
spctl -a -vvv -t install "$PKG" 2>&1 | grep -E "accepted|source"
echo "DONE: $PKG  — ready to ship."
