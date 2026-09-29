#!/bin/bash
# Build a signed + notarized .pkg installer for CUE SAMPLER.
# Installs the (already-notarized) VST3 and AU into the
# system audio plug-in folders, so any DAW finds them.
#
# Prerequisites:
#   - Plugins already signed/notarized/stapled (run ./notarize.sh first).
#   - "Developer ID Installer" cert in keychain (create via Xcode > Accounts).
#   - Notary profile "cue-notary" stored (already done).
#
# Usage:  ./make-installer.sh [version]
#         PKG_TAG=osx11 ./make-installer.sh 1.0.6
set -euo pipefail
cd "$(dirname "$0")"

PROJECT_VERSION="$(sed -nE 's/^[[:space:]]*project\(CueSampler VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)"
VERSION="${1:-$PROJECT_VERSION}"
[ -n "$PROJECT_VERSION" ] && [ "$VERSION" = "$PROJECT_VERSION" ] || {
  echo "ERROR: installer version must match CMakeLists.txt ($PROJECT_VERSION)"; exit 1;
}
PKG_TAG="${PKG_TAG:-}"
if [[ -n "$PKG_TAG" && ! "$PKG_TAG" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "ERROR: PKG_TAG may contain only letters, numbers, dots, underscores, and hyphens"
  exit 1
fi
PKG_SUFFIX="${PKG_TAG:+-${PKG_TAG}}"
INSTALLER_ID="Developer ID Installer: JERRY OTTAVIO VOLPE (KUU9K5SWA8)"
PROFILE="cue-notary"
CUE_NOTARIZE="${CUE_NOTARIZE:-1}"
PKG_ID="com.cuesoftware.cuesampler.installer"

VST3="build/CueSampler_artefacts/Release/VST3/CUE SAMPLER.vst3"
AU="build/CueSampler_artefacts/Release/AU/CUE SAMPLER.component"

# Verify legal resources and binary metadata without modifying signed bundles.
python3 tools/prepare_release_legal.py --verify-bundle "$VST3" --verify-bundle "$AU"
codesign --verify --deep --strict "$VST3"
codesign --verify --deep --strict "$AU"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
OUT="dist"
UNSIGNED="$STAGE/unsigned.pkg"
# URL-safe filename (no spaces) so GitHub Release asset links don't need %20
# escaping. The installer UI title still reads "CUE SAMPLER" (from the
# distribution.xml <title>), independent of this filename.
SIGNED="$OUT/CUESAMPLER-${VERSION}${PKG_SUFFIX}.pkg"
mkdir -p "$OUT"

# --- Stage the bundles into their real install locations -------------------
mkdir -p "$STAGE/root/Library/Audio/Plug-Ins/VST3"
cp -R "$VST3" "$STAGE/root/Library/Audio/Plug-Ins/VST3/"
echo "Staged VST3."

if [ -e "$AU" ]; then
  mkdir -p "$STAGE/root/Library/Audio/Plug-Ins/Components"
  cp -R "$AU" "$STAGE/root/Library/Audio/Plug-Ins/Components/"
  echo "Staged AU."
else
  echo "AU not included (missing)."
fi

# External/non-APFS workspaces can represent macOS metadata as AppleDouble
# sidecars, while pkgbuild serializes copied provenance attributes the same way.
# Neither belongs in the installed plug-ins. Removing extended attributes does
# not alter signed bundle contents; verify both signatures and tickets again so
# packaging stops immediately if that ever changes.
find "$STAGE/root" \( -name '._*' -o -name '.DS_Store' \) -delete
xattr -cr "$STAGE/root"
STAGED_VST3="$STAGE/root/Library/Audio/Plug-Ins/VST3/CUE SAMPLER.vst3"
STAGED_AU="$STAGE/root/Library/Audio/Plug-Ins/Components/CUE SAMPLER.component"
codesign --verify --deep --strict "$STAGED_VST3"
xcrun stapler validate "$STAGED_VST3"
if [ -e "$STAGED_AU" ]; then
  codesign --verify --deep --strict "$STAGED_AU"
  xcrun stapler validate "$STAGED_AU"
fi

# --- Build the component package -------------------------------------------
echo "==> Building component package..."
COMPONENT="$STAGE/component.pkg"
pkgbuild --root "$STAGE/root" \
  --identifier "$PKG_ID" \
  --version "$VERSION" \
  --install-location "/" \
  "$COMPONENT"

# --- Wrap into a product archive that shows the EULA (Agree screen) ---------
echo "==> Building product archive with license..."
LICENSE="LICENSE.txt"
[ -e "$LICENSE" ] || { echo "MISSING: $LICENSE (run: sed ... EULA.md > LICENSE.txt)"; exit 1; }
mkdir -p "$STAGE/resources"
cp "$LICENSE" "$STAGE/resources/LICENSE.txt"
DISTXML="$STAGE/distribution.xml"
cat > "$DISTXML" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>CUE SAMPLER</title>
    <license file="LICENSE.txt"/>
    <pkg-ref id="$PKG_ID"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <choices-outline>
        <line choice="default">
            <line choice="$PKG_ID"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="$PKG_ID" visible="false">
        <pkg-ref id="$PKG_ID"/>
    </choice>
    <pkg-ref id="$PKG_ID" version="$VERSION" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
EOF

productbuild --distribution "$DISTXML" \
  --package-path "$STAGE" \
  --resources "$STAGE/resources" \
  "$UNSIGNED"

# --- Sign the installer (Developer ID Installer cert) ----------------------
echo "==> Signing installer..."
productsign --sign "$INSTALLER_ID" "$UNSIGNED" "$SIGNED"
pkgutil --check-signature "$SIGNED"

# --- Notarize + staple the .pkg --------------------------------------------
if [ "$CUE_NOTARIZE" = "1" ]; then
  echo "==> Notarizing installer (waits for result)..."
  xcrun notarytool submit "$SIGNED" --keychain-profile "$PROFILE" --wait
  xcrun stapler staple "$SIGNED"
  xcrun stapler validate "$SIGNED"
  spctl -a -vvv -t install "$SIGNED"
else
  echo "==> CUE_NOTARIZE=0 — skipping Apple submission, stapling, and Gatekeeper assessment"
fi

# --- Emit a SHA-256 sidecar for release/manual integrity verification ---------
SHA_FILE="${SIGNED}.sha256"
shasum -a 256 "$SIGNED" | awk '{print $1}' > "$SHA_FILE"
echo "==> SHA-256: $(cat "$SHA_FILE")"

rm -rf "$STAGE"
echo
echo "Done: $SIGNED"
echo "      $SHA_FILE"
echo
echo "Publish to GitHub Releases (tag drives the version users compare against):"
echo "  gh release create v${VERSION} \\"
echo "    \"$SIGNED\" \"$SHA_FILE\" \\"
echo "    --title \"CUE SAMPLER ${VERSION}\" --notes \"...\""
