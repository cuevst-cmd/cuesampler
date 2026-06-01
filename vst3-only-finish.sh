#!/bin/bash
# VST3-only finisher: waits for the already-submitted VST3 to clear Apple,
# staples it, then builds the signed+notarized VST3-only .pkg (AU skipped).
set -uo pipefail

PROFILE="cue-notary"
VST3="build/CueSampler_artefacts/Release/VST3/CUE SAMPLER.vst3"
SUB="eff0cee9-7bf5-4820-8422-02706ed5b7b0"

echo "Waiting on Apple for the VST3 (every 30s)..."
while true; do
  S=$(xcrun notarytool info "$SUB" --keychain-profile "$PROFILE" 2>/dev/null \
        | awk '/status:/{print $2; exit}')
  echo "$(date '+%H:%M:%S')  VST3: ${S:-unknown}"
  case "$S" in
    Accepted) break ;;
    Invalid|Rejected)
      echo "VST3 FAILED. Log:"; xcrun notarytool log "$SUB" --keychain-profile "$PROFILE"
      echo "FINISH_EXIT=1"; exit 1 ;;
  esac
  sleep 30
done

echo "==> VST3 accepted. Stapling..."
xcrun stapler staple "$VST3" && xcrun stapler validate "$VST3"

echo "==> Building VST3-only installer (AU not stapled, so make-installer.sh skips it)..."
./make-installer.sh 1.0.0

echo "==> Final installer:"
ls -lh dist/*.pkg
echo "FINISH_EXIT=0"
