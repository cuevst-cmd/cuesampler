#!/bin/bash
# =============================================================================
# release-mac.sh — one-shot signed + notarized macOS installer for CUE SAMPLER.
#
# Runs the whole release in the right order:
#     1. build a fresh universal Release (VST3 + AU)
#     2. sign + notarize + staple the plug-in bundles      (./notarize.sh)
#     3. build + sign + notarize + staple the .pkg + SHA    (./make-installer.sh)
#
# The version is read straight from CMakeLists.txt (project(CUESAMPLER VERSION …))
# so the installer filename, the pkg version, and the app's in-app update check
# can never drift apart.
#
# RUN THIS IN YOUR OWN SHELL (Antigravity / Terminal). It needs your login
# Keychain signing certs (Developer ID Application + Developer ID Installer) and
# the stored notary profile "cue-notary" — it cannot run in a sandbox.
#
# Usage:
#     ./release-mac.sh                 # version auto-read from CMakeLists.txt
#     ./release-mac.sh 1.0.2           # override the version
#     SKIP_BUILD=1 ./release-mac.sh    # re-package existing build/ (no recompile)
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

# --- Version: single source of truth is project(CUESAMPLER VERSION x.y.z) -----
VERSION="${1:-}"
if [ -z "$VERSION" ]; then
  VERSION="$(sed -nE 's/^[[:space:]]*project\(CUESAMPLER[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt | head -1)"
fi
[ -n "$VERSION" ] || { echo "ERROR: could not read version from CMakeLists.txt — pass it explicitly: ./release-mac.sh 1.0.1"; exit 1; }

APP_ID="Developer ID Application: JERRY OTTAVIO VOLPE (KUU9K5SWA8)"
INSTALLER_ID="Developer ID Installer: JERRY OTTAVIO VOLPE (KUU9K5SWA8)"
PKG="dist/CUESAMPLER-${VERSION}.pkg"

echo "=================================================================="
echo "  CUE SAMPLER — macOS release"
echo "    version     : $VERSION"
echo "    skip build  : ${SKIP_BUILD:-0}"
echo "    output      : $PKG"
echo "=================================================================="

# --- Pre-flight: tools, license, and both signing certs -----------------------
command -v cmake >/dev/null || { echo "ERROR: cmake not on PATH"; exit 1; }
command -v xcrun >/dev/null || { echo "ERROR: Xcode command-line tools (xcrun) not found"; exit 1; }
[ -e LICENSE.txt ] || { echo "ERROR: LICENSE.txt missing (installer license screen needs it)"; exit 1; }

security find-identity -v -p codesigning | grep -q "$APP_ID" \
  || { echo "ERROR: signing cert not found in keychain: $APP_ID"; \
       echo "       Install it via Xcode > Settings > Accounts > Manage Certificates."; exit 1; }
security find-identity -v | grep -q "$INSTALLER_ID" \
  || { echo "ERROR: signing cert not found in keychain: $INSTALLER_ID"; \
       echo "       Create a 'Developer ID Installer' certificate in your Apple Developer account."; exit 1; }

# --- 1. Fresh universal Release build (VST3 + AU) -----------------------------
if [ "${SKIP_BUILD:-0}" = "1" ]; then
  echo "==> SKIP_BUILD=1 — reusing the binaries already in build/"
else
  echo "==> Configuring + building Release (arm64 + x86_64)…"
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build --target CueSampler_VST3 CueSampler_AU --parallel
fi

VST3="build/CueSampler_artefacts/Release/VST3/CUE SAMPLER.vst3"
AU="build/CueSampler_artefacts/Release/AU/CUE SAMPLER.component"
[ -d "$VST3" ] || { echo "ERROR: VST3 not found after build: $VST3"; exit 1; }
[ -d "$AU"   ] || { echo "ERROR: AU not found after build: $AU"; exit 1; }

# --- 2. Sign + notarize + staple the plug-in bundles --------------------------
# (notarize.sh signs nested dylibs inside-out, hardened-runtime-signs each
#  bundle, then submits to Apple and staples the ticket. It blocks until Apple
#  returns a result.)
echo "==> Signing + notarizing the plug-ins (./notarize.sh)…"
./notarize.sh

# --- 3. Build + sign + notarize + staple the .pkg installer -------------------
# (make-installer.sh stages the notarized bundles into /Library/Audio/Plug-Ins,
#  builds the component + product archive with the EULA, productsigns it with the
#  Installer cert, notarizes + staples the .pkg, and writes the .sha256 sidecar.)
echo "==> Building + signing + notarizing the installer (./make-installer.sh $VERSION)…"
./make-installer.sh "$VERSION"

# --- Done ---------------------------------------------------------------------
echo
echo "=================================================================="
echo "  DONE — Gatekeeper-clean installer ready to ship:"
ls -lh "$PKG" "${PKG}.sha256" 2>/dev/null || true
echo
echo "  Publish to GitHub Releases (the tag is what the in-app updater"
echo "  compares against, so it must match the version above):"
echo "    gh release create v${VERSION} \"$PKG\" \"${PKG}.sha256\" \\"
echo "      --title \"CUE SAMPLER ${VERSION}\" --notes \"…\""
echo "=================================================================="
