; Inno Setup Script for CUE SAMPLER VST3 & Standalone

#define MyAppName "CUE SAMPLER"
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define MyAppPublisher "CUE SOFTWARE"
#define MyAppURL "https://cuesampler.com"
#define MyAppExeName "CUE SAMPLER.exe"

[Setup]
AppId={{com.cuesoftware.cuesampler}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppPublisher}\{#MyAppName}
DefaultGroupName={#MyAppPublisher}\{#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=LICENSE.txt
OutputDir=dist
OutputBaseFilename=CUESAMPLER-Setup-{#MyAppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation (VST3 + Standalone Application)"
Name: "vst3only"; Description: "VST3 Plugin only"
Name: "standaloneonly"; Description: "Standalone Application only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 Plugin (64-bit)"; Types: full vst3only custom; Flags: fixed
Name: "standalone"; Description: "Standalone Application (64-bit)"; Types: full standaloneonly custom

[Files]
; --- VST3 Files ---
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\CUE SAMPLER.vst3"; DestDir: "{commoncf}\VST3"; Components: vst3; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\onnxruntime.dll"; DestDir: "{commoncf}\VST3"; Components: vst3; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\DirectML.dll"; DestDir: "{commoncf}\VST3"; Components: vst3; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\beat_this.onnx"; DestDir: "{commoncf}\VST3"; Components: vst3; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\beat_this.onnx.data"; DestDir: "{commoncf}\VST3"; Components: vst3; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\Contents\x86_64-win\htdemucs\htdemucs.onnx"; DestDir: "{commoncf}\VST3\htdemucs"; Components: vst3; Flags: ignoreversion

; --- Standalone Files ---
Source: "build\CueSampler_artefacts\Release\Standalone\CUE SAMPLER.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\Standalone\onnxruntime.dll"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\Standalone\DirectML.dll"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\Standalone\beat_this.onnx"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\Standalone\beat_this.onnx.data"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "build\CueSampler_artefacts\Release\Standalone\htdemucs\htdemucs.onnx"; DestDir: "{app}\htdemucs"; Components: standalone; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: standalone
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon; Components: standalone

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: standalone

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent; Components: standalone

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
