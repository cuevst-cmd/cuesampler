# CUE SAMPLER 1.1.0 for Windows

Improves editor responsiveness during stem separation on Windows. Separation now uses CPU by default, reserves CPU headroom, disables ONNX worker busy-waiting, and runs the separation caller at low priority.

DirectML GPU acceleration remains available by setting `CUE_ENABLE_DIRECTML=1` before starting the host. `CUE_DISABLE_DIRECTML` takes precedence. CPU separation may take longer than GPU inference.

The installer supports Windows 10/11 x64 and installs the VST3 plugin, the stem and beat-analysis models, runtime dependencies, and third-party notices. The standalone application is not included.

The Windows installer is unsigned, matching previous Windows packages. Windows may display an Unknown Publisher or SmartScreen prompt.

Validation includes the six Windows regression checks and an actual-model separation test with an editor open, message-dispatch measurements, and exact separated-mix save/restore.

This release contains the Windows installer and its SHA-256 checksum sidecar.

Download: `CUESAMPLER-Setup-1.1.0-UNSIGNED.exe` (217,457,111 bytes).

SHA-256: `7dec9341abfcdc9505f3652aa1752f9ca0ebc43939b97b636f691641b59e48af`.
