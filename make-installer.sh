#!/bin/bash
# Build a signed + notarized .pkg installer for CUE SAMPLER.
# Installs the (already-notarized) VST3 — and the AU if present — into the
# system audio plug-in folders, so any DAW finds them.
#
# Prerequisites:
#   - Plugins already signed/notarized/stapled (run ./notarize.sh first).
#   - "Developer ID Installer" cert in keychain (create via Xcode > Accounts).
#   - Notary profile "cue-notary" stored (already done).
#
# Usage:  ./make-installer.sh [version]
set -euo pipefail

VERSION="${1:-1.0.0}"
INSTALLER_ID="Developer ID Installer: JERRY OTTAVIO VOLPE (KUU9K5SWA8)"
PROFILE="cue-notary"
PKG_ID="com.cuesoftware.cuesampler.installer"

VST3="build/CueSampler_artefacts/Release/VST3/CUE SAMPLER.vst3"
AU="build/CueSampler_artefacts/Release/AU/CUE SAMPLER.component"

STAGE="$(mktemp -d)"
OUT="dist"
UNSIGNED="$STAGE/unsigned.pkg"
# URL-safe filename (no spaces) so GitHub Release asset links don't need %20
# escaping. The installer UI title still reads "CUE SAMPLER" (from the
# distribution.xml <title>), independent of this filename.
SIGNED="$OUT/CUESAMPLER-${VERSION}.pkg"
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
  --resources "$(pwd)" \
  "$UNSIGNED"

# --- Sign the installer (Developer ID Installer cert) ----------------------
echo "==> Signing installer..."
productsign --sign "$INSTALLER_ID" "$UNSIGNED" "$SIGNED"
pkgutil --check-signature "$SIGNED"

# --- Notarize + staple the .pkg --------------------------------------------
echo "==> Notarizing installer (waits for result)..."
if xcrun notarytool submit "$SIGNED" --keychain-profile "$PROFILE" --wait; then
  echo "==> Stapling ticket to installer..."
  xcrun stapler staple "$SIGNED"
  xcrun stapler validate "$SIGNED"
else
  echo "WARNING: Notarization submission failed (e.g. pending Apple Developer agreement). The installer is fully code-signed with your Developer ID certificate."
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
spctl -a -vvv -t install "$SIGNED" 2>&1 || true
