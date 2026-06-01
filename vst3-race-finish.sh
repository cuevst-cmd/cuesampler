#!/bin/bash
# Watches BOTH VST3 notarization submissions; whichever is Accepted first wins.
# Then staples the VST3 and builds the VST3-only .pkg (AU skipped).
set -uo pipefail

PROFILE="cue-notary"
VST3="build/CueSampler_artefacts/Release/VST3/CUE SAMPLER.vst3"
SUBS=("eff0cee9-7bf5-4820-8422-02706ed5b7b0" "13aaa6e8-a937-4dbc-8b23-f04edd7ecb3a")

echo "Watching ${#SUBS[@]} submissions (every 30s)..."
WINNER=""
while [ -z "$WINNER" ]; do
  for SUB in "${SUBS[@]}"; do
    S=$(xcrun notarytool info "$SUB" --keychain-profile "$PROFILE" 2>/dev/null \
          | awk '/status:/{print $2; exit}')
    echo "$(date '+%H:%M:%S')  ${SUB:0:8}: ${S:-unknown}"
    case "$S" in
      Accepted) WINNER="$SUB"; break ;;
      Invalid|Rejected)
        echo "  ${SUB:0:8} FAILED — log:"; xcrun notarytool log "$SUB" --keychain-profile "$PROFILE" ;;
    esac
  done
  [ -z "$WINNER" ] && sleep 30
done

echo "==> WINNER: $WINNER accepted. Stapling..."
xcrun stapler staple "$VST3" && xcrun stapler validate "$VST3"

echo "==> Building VST3-only installer..."
./make-installer.sh 1.0.0

echo "==> Final installer:"
ls -lh dist/*.pkg
echo "FINISH_EXIT=0"
