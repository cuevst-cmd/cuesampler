# CUE SAMPLER 1.0.9 (Beta)

## What's New & Improvements

- **Universal macOS Installer**: Signed with Developer ID, notarized and stapled by Apple for macOS 11+ (Intel & Apple Silicon universal binary). Installs VST3 and AU.
- **Windows x64 Installer**: NSIS VST3 installer with DirectML and ONNX Runtime support for Windows 10/11 64-bit.
- **Policy & Licensing Updates**: Updated canonical EULA and Privacy Policy, third-party notices, and matching source archive in application and installer resources.
- **Model & Initialization Hardening**: Added explicit telemetry-disable calls on ONNX environment initialization.
- **Stability & Performance**: Bug fixes and stability improvements across audio playback, slicing, and session restore.

## Downloads

| Platform | File | SHA-256 |
|---|---|---|
| macOS | `CUESAMPLER-1.0.9.pkg` | `3ae70d1ec59baa0d755f4225380f6e787dd8821ee144e8685e332ca4fd8d21e5` |
| Windows | `CUESAMPLER-Setup-1.0.9-BETA-UNSIGNED.exe` | `f6259c323ada47dbd99ec7983b07571bb19d22f2c4d65ed21b9c89adf2d1bf8f` |

Each installer ships with a `.sha256` sidecar file to verify download integrity.

## Compatibility

- **macOS 11 (Big Sur) or newer**: Universal binary (Apple Silicon and Intel x86_64). Installs **VST3** and **Audio Unit (AU)**. Signed with Developer ID, notarized and stapled by Apple.
- **Windows 10 / 11 (64-bit)**: Installs **VST3**.
  > **Note**: The Windows installer is currently unsigned. Windows SmartScreen may show an "Unknown Publisher" prompt; click **More info -> Run anyway** to proceed.
