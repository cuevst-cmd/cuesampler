; Inno Setup Script for CUE SAMPLER - VST3 plugin (64-bit) only.

#define MyAppName "CUE SAMPLER"
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define MyAppPublisher "CUE SOFTWARE"
#define MyAppURL "https://cuesampler.com"
#define MyAppCopyright "Copyright (C) 2026 CUE SOFTWARE"

[Setup]
AppId={{com.cuesoftware.cuesampler}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright={#MyAppCopyright}
DefaultDirName={autopf}\{#MyAppPublisher}\{#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=yes
LicenseFile=LICENSE.txt
OutputDir=dist
OutputBaseFilename=CUESAMPLER-Setup-{#MyAppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayName={#MyAppName}

; Metadata embedded in the generated Setup .exe. This populates the file's
; Properties > Details (Company / Product / Copyright) and the publisher shown
; in Apps & Features, so the installer no longer presents blank/unknown details.
;
; IMPORTANT: this does NOT remove the Windows "Unknown Publisher" warning in the
; UAC elevation prompt / SmartScreen. That trust banner is driven solely by an
; Authenticode code-signing signature, which is intentionally deferred for now.
; When a signing certificate is added later, sign dist\CUESAMPLER-Setup-*.exe
; (signtool) and the warning goes away with no further changes here.
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
VersionInfoCopyright={#MyAppCopyright}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; --- VST3 plugin (64-bit) + its runtime DLLs and bundled ONNX models ---
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\CUE SAMPLER.vst3"; DestDir: "{commoncf}\VST3"; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\onnxruntime.dll"; DestDir: "{commoncf}\VST3"; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\DirectML.dll"; DestDir: "{commoncf}\VST3"; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\beat_this.onnx"; DestDir: "{commoncf}\VST3"; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\beat_this.onnx.data"; DestDir: "{commoncf}\VST3"; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\htdemucs\htdemucs.onnx"; DestDir: "{commoncf}\VST3\htdemucs"; Flags: ignoreversion

[UninstallDelete]
; The .vst3 and DLLs are tracked and removed automatically; clean up the bundled
; model folder so no empty/leftover directory remains under the shared VST3 path.
Type: filesandordirs; Name: "{commoncf}\VST3\htdemucs"
