; NSIS installer for CUE SAMPLER - VST3 plugin (64-bit) only.

Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"

!define APP_NAME "CUE SAMPLER"
!define APP_PUBLISHER "CUE SOFTWARE"
!define APP_URL "https://cuesampler.com"
!define APP_COPYRIGHT "Copyright (C) 2026 CUE SOFTWARE"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\com.cuesoftware.cuesampler"
!define LEGACY_INNO_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\{com.cuesoftware.cuesampler}_is1"

!ifndef MyAppVersion
  !define MyAppVersion "1.0.0"
!endif

!ifndef MyBuildDir
  !define MyBuildDir "build"
!endif

Name "${APP_NAME} ${MyAppVersion}"
OutFile "dist\CUESAMPLER-Setup-${MyAppVersion}.exe"
InstallDir "$PROGRAMFILES64\${APP_PUBLISHER}\${APP_NAME}"
InstallDirRegKey HKLM "${UNINSTALL_KEY}" "InstallLocation"
RequestExecutionLevel admin
ManifestSupportedOS Win10
SetCompressor /SOLID lzma
SetCompressorDictSize 64
BrandingText "${APP_NAME} Setup"

VIProductVersion "${MyAppVersion}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${MyAppVersion}"
VIAddVersionKey /LANG=1033 "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${MyAppVersion}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "${APP_COPYRIGHT}"

; A commercial release passes an Authenticode command from the release script.
; NSIS runs it against the generated uninstaller before embedding that file in
; Setup.exe, preventing an Unknown Publisher prompt during product removal.
!ifdef MySignUninstaller
  !uninstfinalize '$%CUE_NSIS_SIGN_COMMAND%' = 0
!endif

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_NOAUTOCLOSE
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} requires Windows 10 or Windows 11."
    Abort
  ${EndIf}
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} requires 64-bit Windows 10 or Windows 11."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "CUE SAMPLER VST3" SEC_VST3
  SectionIn RO
  SetRegView 64
  SetShellVarContext all

  ; CueSampler and ONNX Runtime use the dynamic MSVC runtime. The Microsoft
  ; redistributable safely repairs or updates an existing compatible runtime.
  SetOutPath "$TEMP\CUESAMPLER-Setup"
  File /oname=vc_redist.x64.exe "${MyBuildDir}\prerequisites\vc_redist.x64.exe"
  DetailPrint "Installing Microsoft Visual C++ x64 runtime..."
  ExecWait '"$TEMP\CUESAMPLER-Setup\vc_redist.x64.exe" /install /quiet /norestart' $0
  ${If} $0 = 3010
    SetRebootFlag true
  ${ElseIf} $0 = 1641
    SetRebootFlag true
  ${ElseIf} $0 = 1638
    DetailPrint "A compatible or newer Visual C++ runtime is already installed."
  ${ElseIf} $0 <> 0
    MessageBox MB_OK|MB_ICONSTOP "Microsoft Visual C++ runtime installation failed with exit code $0. CUE SAMPLER setup cannot continue."
    Abort
  ${EndIf}
  Delete "$TEMP\CUESAMPLER-Setup\vc_redist.x64.exe"
  RMDir "$TEMP\CUESAMPLER-Setup"

  ; Migrate machines that installed the earlier Inno Setup package. Leaving its
  ; uninstaller registered would let it delete a later NSIS-managed VST3 bundle.
  ReadRegStr $1 HKLM "${LEGACY_INNO_KEY}" "QuietUninstallString"
  ${If} $1 != ""
    DetailPrint "Removing the legacy CUE SAMPLER installer registration..."
    ExecWait '$1 /SUPPRESSMSGBOXES /NORESTART' $2
    ${If} $2 <> 0
      MessageBox MB_OK|MB_ICONSTOP "The previous CUE SAMPLER version could not be removed (exit code $2). Close all DAWs and run setup again."
      Abort
    ${EndIf}
  ${EndIf}

  ; Replace only CueSampler's private bundle so upgrades cannot leave stale
  ; models or runtime libraries behind.
  RMDir /r "$COMMONFILES64\VST3\CUE SAMPLER.vst3"
  SetOutPath "$COMMONFILES64\VST3\CUE SAMPLER.vst3"
  File /r "${MyBuildDir}\CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3\*.*"

  ; Notices and the complete modified Bungee source accompany every copy.
  SetOutPath "$COMMONFILES64\VST3\CUE SAMPLER.vst3\Contents\Resources\Licenses"
  File /r "${MyBuildDir}\release-notices\*.*"

  SetOutPath "$INSTDIR"
  File "LICENSE.txt"
  File "THIRD_PARTY_NOTICES.txt"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${MyAppVersion}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "${APP_URL}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all

  ; Remove only paths and registry data owned by CUE SAMPLER.
  RMDir /r "$COMMONFILES64\VST3\CUE SAMPLER.vst3"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  RMDir "$PROGRAMFILES64\${APP_PUBLISHER}"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
SectionEnd
