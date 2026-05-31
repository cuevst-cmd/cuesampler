#!/bin/bash
# Run this in your OWN Terminal:  cd ~/Documents/SAMPLERv3 && ./finish.sh
#
# Waits for Apple to finish notarizing the AU (which is stuck in their slow
# queue), staples it, then builds the final signed+notarized installer
# containing BOTH the VST3 and the AU. Survives because it's your shell.
set -uo pipefail

PROFILE="cue-notary"
AU="build/CueSampler_artefacts/Release/AU/CUE SAMPLER.component"
AU_SUB="df2bb119-562f-44c1-8435-c1c677ce300d"   # existing AU submission

echo "Waiting on Apple for the AU (checking every 60s)..."
while true; do
  S=$(xcrun notarytool info "$AU_SUB" --keychain-profile "$PROFILE" 2>/dev/null \
        | awk '/status:/{print $2; exit}')
  echo "$(date '+%H:%M:%S')  AU: ${S:-unknown}"
  case "$S" in
    Accepted) break ;;
    Invalid|Rejected)
      echo "AU FAILED. Log:"; xcrun notarytool log "$AU_SUB" --keychain-profile "$PROFILE"
      exit 1 ;;
  esac
  sleep 60
done

echo "==> AU accepted. Stapling..."
xcrun stapler staple "$AU" && xcrun stapler validate "$AU"

echo "==> Building final combined installer (VST3 + AU)..."
./make-installer.sh 1.0.0

echo
echo "ALL DONE. Final installer:"
ls -lh dist/*.pkg
