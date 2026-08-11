# PowerShell script to build the Windows installer for CUE SAMPLER.
# Generates the installer executable and a SHA-256 checksum sidecar file.
#
# Development build:
#   .\make-installer-windows.ps1 [-Version "1.0.7"] [-BuildDir "build"]
# Commercial release (requires confirmed JUCE plan eligibility and a trusted certificate):
#   .\make-installer-windows.ps1 -CommercialRelease `
#       -JuceLicenseEligibilityConfirmed `
#       -SigningCertificateThumbprint "<SHA1 thumbprint>"
# When -Version is omitted, it is read from project(CueSampler VERSION ...) in
# CMakeLists.txt so the plugin and installer cannot silently drift apart.

param (
    [string]$Version = "",
    [string]$BuildDir = "build",
    [switch]$CommercialRelease,
    [Alias("JuceCommercialLicenseConfirmed")]
    [switch]$JuceLicenseEligibilityConfirmed,
    [string]$SigningCertificateThumbprint = "",
    [string]$TimestampUrl = "https://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

function Find-SignTool {
    $windowsKitsBin = "C:\Program Files (x86)\Windows Kits\10\bin"
    if (-not (Test-Path $windowsKitsBin)) {
        return $null
    }

    return Get-ChildItem -Path $windowsKitsBin -Filter "signtool.exe" -Recurse -File |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

function Invoke-ReleaseSigning {
    param (
        [Parameter(Mandatory = $true)][string]$Path
    )

    $signArgs = @(
        "sign",
        "/sha1", $script:ReleaseCertificate.Thumbprint,
        "/s", "My",
        "/fd", "SHA256",
        "/tr", $TimestampUrl,
        "/td", "SHA256"
    )
    if ($script:ReleaseCertificateStore -eq "LocalMachine") {
        $signArgs += "/sm"
    }
    $signArgs += $Path

    Write-Host "==> Authenticode signing: $Path"
    & $script:SignTool @signArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "SignTool failed while signing: $Path"
    }

    & $script:SignTool verify /pa /all /tw $Path
    if ($LASTEXITCODE -ne 0) {
        Write-Error "SignTool could not verify the timestamped signature: $Path"
    }
}

if ($CommercialRelease) {
    if (-not $JuceLicenseEligibilityConfirmed) {
        Write-Error "Commercial release blocked: confirm eligibility for the applicable JUCE 8 plan (including free Starter, when eligible) with -JuceLicenseEligibilityConfirmed."
    }
    if ([string]::IsNullOrWhiteSpace($SigningCertificateThumbprint)) {
        Write-Error "Commercial release blocked: provide a trusted Authenticode code-signing certificate with -SigningCertificateThumbprint."
    }

    $normalizedThumbprint = $SigningCertificateThumbprint -replace '\s', ''
    $script:ReleaseCertificate = $null
    $script:ReleaseCertificateStore = $null
    foreach ($storeName in @("CurrentUser", "LocalMachine")) {
        $candidate = Get-ChildItem "Cert:\$storeName\My" -ErrorAction SilentlyContinue |
            Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
            Select-Object -First 1
        if ($null -ne $candidate) {
            $script:ReleaseCertificate = $candidate
            $script:ReleaseCertificateStore = $storeName
            break
        }
    }

    if ($null -eq $script:ReleaseCertificate -or -not $script:ReleaseCertificate.HasPrivateKey) {
        Write-Error "Commercial release blocked: the requested certificate was not found with a private key in CurrentUser\\My or LocalMachine\\My."
    }

    $codeSigningOid = "1.3.6.1.5.5.7.3.3"
    $hasCodeSigningEku = $script:ReleaseCertificate.Extensions |
        Where-Object { $_.Oid.Value -eq "2.5.29.37" } |
        ForEach-Object { $_.EnhancedKeyUsages } |
        Where-Object { $_.Value -eq $codeSigningOid }
    if ($null -eq $hasCodeSigningEku) {
        Write-Error "Commercial release blocked: the selected certificate is not valid for code signing."
    }

    $script:SignTool = Find-SignTool
    if ([string]::IsNullOrWhiteSpace($script:SignTool)) {
        Write-Error "Commercial release blocked: SignTool was not found in the Windows SDK."
    }
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    $projectLine = Select-String -Path "CMakeLists.txt" -Pattern 'project\(CueSampler VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)' | Select-Object -First 1
    if ($null -eq $projectLine) {
        Write-Error "Could not read the CueSampler version from CMakeLists.txt."
    }
    $Version = $projectLine.Matches[0].Groups[1].Value
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "Installer version must contain exactly three numeric components (for example, 1.0.7)."
}

$BuildDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $BuildDir))

Write-Host "==> Preparing Windows Installer (Version: $Version, BuildDir: $BuildDir)..."

# --- 1. Generate LICENSE.txt from EULA.md ---
if (Test-Path "EULA.md") {
    Write-Host "==> Generating LICENSE.txt from EULA.md..."
    $eula = Get-Content -Path "EULA.md" -Raw
    # Strip basic markdown headers and bolding
    $license = $eula -replace '(?m)^#\s+', '' -replace '(?m)^##\s+', '' -replace '\*\*', ''
    # Keep the generated tracked file byte-stable on macOS, Windows, and CI.
    $licenseText = (($license -replace "`r`n", "`n") -replace "`r", "`n").TrimEnd() + "`n"
    $utf8WithBom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText((Join-Path (Get-Location) "LICENSE.txt"), $licenseText, $utf8WithBom)
    Write-Host "    LICENSE.txt created."
} else {
    if (-not (Test-Path "LICENSE.txt")) {
        Write-Error "EULA.md not found and LICENSE.txt is missing. Cannot proceed."
    }
    Write-Host "    EULA.md missing, using existing LICENSE.txt."
}

# --- 2. Verify Release Build Artifacts Exist ---
# VST3-only release: the standalone app is intentionally NOT shipped in the installer.
$vst3Path = Join-Path $BuildDir "CueSampler_artefacts\Release\VST3\CUE SAMPLER.vst3"

if (-not (Test-Path $vst3Path)) {
    Write-Error "Required VST3 binary not found at: $vst3Path`nPlease build the CueSampler_VST3 target in Release mode first."
}

$requiredRuntimeFiles = @(
    "Contents\x86_64-win\CUE SAMPLER.vst3",
    "Contents\x86_64-win\onnxruntime.dll",
    "Contents\x86_64-win\DirectML.dll",
    "Contents\x86_64-win\beat_this.onnx",
    "Contents\x86_64-win\beat_this.onnx.data"
)

foreach ($relativePath in $requiredRuntimeFiles) {
    $requiredPath = Join-Path $vst3Path $relativePath
    if (-not (Test-Path $requiredPath -PathType Leaf)) {
        Write-Error "Required VST3 runtime file not found: $requiredPath"
    }
}

$stemModelPath = Join-Path $vst3Path "Contents\x86_64-win\htdemucs\htdemucs.onnx"
if (-not (Test-Path $stemModelPath -PathType Leaf)) {
    if ($CommercialRelease) {
        Write-Error "Commercial release blocked: HTDemucs is missing from the VST3 bundle, so the advertised stem-separation feature would be disabled. Run download-htdemucs-model.ps1, reconfigure CMake, and rebuild CueSampler_VST3."
    }
    Write-Warning "HTDemucs model is absent. The installer will work, but stem separation will be disabled."
} else {
    # Pin the parity-verified fp32 model published by StemSplitio. Hugging Face's
    # X-Linked-ETag is the Git LFS SHA-256 for this exact 316,446,953-byte file.
    $expectedStemModelSha256 = "68d0bf16428ef66e692cdff8a9ccf28f1ef3f69440d57e58605a4cc55fcc5e74"
    $actualStemModelSha256 = (Get-FileHash -Path $stemModelPath -Algorithm SHA256).Hash.ToLower()
    if ($actualStemModelSha256 -ne $expectedStemModelSha256) {
        Write-Error "HTDemucs integrity check failed for $stemModelPath. Expected $expectedStemModelSha256, got $actualStemModelSha256."
    }
    Write-Host "    HTDemucs model verified: $actualStemModelSha256"
}

# --- 3. Stage third-party notices and MPL source form ---
# Bungee is MPL-2.0 and is patched by this project. Ship its complete modified
# source tree (without git metadata) so every installer recipient can obtain the
# corresponding Source Code Form directly, without depending on a future URL.
$releaseNoticesDir = Join-Path $BuildDir "release-notices"
New-Item -ItemType Directory -Path $releaseNoticesDir -Force | Out-Null

$noticeFiles = @(
    @{ Source = "THIRD_PARTY_NOTICES.txt"; Destination = "THIRD_PARTY_NOTICES.txt" },
    @{ Source = "licenses\Beat-This-MIT.txt"; Destination = "Beat-This-MIT.txt" },
    @{ Source = "licenses\Demucs-MIT.txt"; Destination = "Demucs-MIT.txt" },
    @{ Source = "assets\Syne-OFL.txt"; Destination = "Syne-OFL-1.1.txt" },
    @{ Source = (Join-Path $BuildDir "_deps\juce-src\LICENSE.md"); Destination = "JUCE-LICENSE.md" },
    @{ Source = (Join-Path $BuildDir "_deps\bungee-src\LICENSE"); Destination = "Bungee-MPL-2.0.txt" },
    @{ Source = (Join-Path $BuildDir "_deps\onnxruntime-src\LICENSE"); Destination = "ONNX-Runtime-MIT.txt" },
    @{ Source = (Join-Path $BuildDir "_deps\onnxruntime-src\ThirdPartyNotices.txt"); Destination = "ONNX-Runtime-ThirdPartyNotices.txt" },
    @{ Source = (Join-Path $BuildDir "_deps\directml-src\LICENSE.txt"); Destination = "DirectML-LICENSE.txt" },
    @{ Source = (Join-Path $BuildDir "_deps\directml-src\ThirdPartyNotices.txt"); Destination = "DirectML-ThirdPartyNotices.txt" }
)

foreach ($notice in $noticeFiles) {
    if (-not (Test-Path $notice.Source -PathType Leaf)) {
        Write-Error "Required release notice is missing: $($notice.Source)"
    }
    Copy-Item -Path $notice.Source -Destination (Join-Path $releaseNoticesDir $notice.Destination) -Force
}

$bungeeSourceDir = Join-Path $BuildDir "_deps\bungee-src"
$bungeeSourceArchive = Join-Path $releaseNoticesDir "Bungee-7354c0c-modified-source.zip"
if (Test-Path $bungeeSourceArchive) {
    Remove-Item -LiteralPath $bungeeSourceArchive -Force
}
& tar.exe -a -c -f $bungeeSourceArchive --exclude=.git -C $bungeeSourceDir .
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bungeeSourceArchive -PathType Leaf)) {
    Write-Error "Failed to create the Bungee MPL source archive."
}
Write-Host "    Third-party notices and Bungee source staged: $releaseNoticesDir"

# Sign the product binary before NSIS packages the VST3 bundle. Microsoft and
# third-party runtime DLLs retain their publishers' existing signatures.
if ($CommercialRelease) {
    Invoke-ReleaseSigning -Path (Join-Path $vst3Path "Contents\x86_64-win\CUE SAMPLER.vst3")
}

# --- 4. Stage the matching Microsoft Visual C++ x64 Redistributable ---
# CueSampler and ONNX Runtime both link the dynamic VC runtime. Prefer the
# redistributable installed with the active MSVC toolchain; CI can fall back to
# Microsoft's stable VS 2022 permalink. Verify the publisher before packaging.
$prerequisiteDir = Join-Path $BuildDir "prerequisites"
if (-not (Test-Path $prerequisiteDir)) {
    New-Item -ItemType Directory -Path $prerequisiteDir | Out-Null
}

$vcRedistDestination = Join-Path $prerequisiteDir "vc_redist.x64.exe"
$vcRedistCandidates = @()

if (-not [string]::IsNullOrWhiteSpace($env:VCToolsRedistDir)) {
    $vcRedistCandidates += (Join-Path $env:VCToolsRedistDir "vc_redist.x64.exe")
}

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstallPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not [string]::IsNullOrWhiteSpace($vsInstallPath)) {
        $vcRedistCandidates += (Join-Path $vsInstallPath "VC\Redist\MSVC\v143\vc_redist.x64.exe")
    }
}

$vcRedistSource = $vcRedistCandidates | Where-Object { Test-Path $_ -PathType Leaf } | Select-Object -First 1
if ($null -ne $vcRedistSource) {
    Copy-Item -Path $vcRedistSource -Destination $vcRedistDestination -Force
} else {
    Write-Host "==> Downloading Microsoft Visual C++ x64 Redistributable..."
    Invoke-WebRequest -Uri "https://aka.ms/vs/17/release/vc_redist.x64.exe" -OutFile $vcRedistDestination
}

$vcRedistSignature = Get-AuthenticodeSignature $vcRedistDestination
if ($vcRedistSignature.Status -ne "Valid" -or $vcRedistSignature.SignerCertificate.Subject -notmatch "Microsoft Corporation") {
    Write-Error "Microsoft Visual C++ Redistributable signature validation failed."
}

Write-Host "    VC++ Redistributable staged: $vcRedistDestination"

# --- 5. Locate the NSIS compiler (makensis.exe) ---
$makensis = "makensis.exe"
$commonPaths = @(
    "C:\Program Files (x86)\NSIS\makensis.exe",
    "C:\Program Files\NSIS\makensis.exe",
    "$env:LOCALAPPDATA\Programs\NSIS\makensis.exe"
)

$found = $false
foreach ($path in $commonPaths) {
    if (Test-Path $path) {
        $makensis = $path
        $found = $true
        break
    }
}

if (-not $found) {
    # Check if makensis is in the environment PATH.
    if ($null -ne (Get-Command $makensis -ErrorAction SilentlyContinue)) {
        $found = $true
    }
}

if (-not $found) {
    Write-Error "NSIS (makensis.exe) was not found in standard paths or in PATH. Install NSIS 3.12 or newer from https://nsis.sourceforge.io/Download."
}

Write-Host "==> Using NSIS compiler: $makensis"

# --- 6. Create output folder ---
if (-not (Test-Path "dist")) {
    New-Item -ItemType Directory -Path "dist" | Out-Null
}

# --- 7. Compile the Installer ---
Write-Host "==> Compiling installer..."
$compilerArgs = @(
    "/WX",
    "/DMyAppVersion=$Version",
    "/DMyBuildDir=$BuildDir"
)

if ($CommercialRelease) {
    # The final Setup.exe is signed after compilation below. The embedded
    # uninstaller must be signed during NSIS compilation, before it is packed.
    $uninstallerSignParts = @(
        ('"{0}"' -f $script:SignTool),
        "sign",
        "/sha1", $script:ReleaseCertificate.Thumbprint,
        "/s", "My",
        "/fd", "SHA256",
        "/tr", ('"{0}"' -f $TimestampUrl),
        "/td", "SHA256"
    )
    if ($script:ReleaseCertificateStore -eq "LocalMachine") {
        $uninstallerSignParts += "/sm"
    }
    $uninstallerSignParts += '"%1"'
    $uninstallerSignCommand = $uninstallerSignParts -join " "
    # makensis reparses native command-line quoting, which splits a /D value at
    # the space in "Program Files". A process-scoped environment value preserves
    # the complete SignTool command exactly; the NSIS script expands it only for
    # !uninstfinalize.
    $previousNsisSignCommand = $env:CUE_NSIS_SIGN_COMMAND
    $env:CUE_NSIS_SIGN_COMMAND = $uninstallerSignCommand
    $compilerArgs += "/DMySignUninstaller=1"
}

$compilerArgs += "installer.nsi"
try {
    $compilerOutput = & $makensis @compilerArgs 2>&1
    $compilerExitCode = $LASTEXITCODE
} finally {
    if ($CommercialRelease) {
        if ($null -eq $previousNsisSignCommand) {
            Remove-Item Env:CUE_NSIS_SIGN_COMMAND -ErrorAction SilentlyContinue
        } else {
            $env:CUE_NSIS_SIGN_COMMAND = $previousNsisSignCommand
        }
    }
}
$compilerOutput | ForEach-Object { Write-Host $_ }

if ($compilerExitCode -ne 0) {
    Write-Error "NSIS failed with exit code $compilerExitCode."
}

$compiledSetupFile = "dist\CUESAMPLER-Setup-$Version.exe"
if ($CommercialRelease) {
    $setupFile = $compiledSetupFile
} else {
    $setupFile = "dist\CUESAMPLER-Setup-$Version-UNSIGNED.exe"
    if (-not (Test-Path $compiledSetupFile -PathType Leaf)) {
        Write-Error "Compiled installer executable was not found at: $compiledSetupFile"
    }
    Move-Item -LiteralPath $compiledSetupFile -Destination $setupFile -Force
    if (Test-Path "$compiledSetupFile.sha256" -PathType Leaf) {
        Remove-Item -LiteralPath "$compiledSetupFile.sha256" -Force
    }
    Write-Warning "Created an unsigned development candidate. Do not sell or publish it as the commercial release."
}

# --- 8. Sign the release installer and generate its SHA-256 sidecar ---
if (Test-Path $setupFile) {
    if ($CommercialRelease) {
        Invoke-ReleaseSigning -Path $setupFile
    }
    Write-Host "==> Generating SHA-256 checksum sidecar..."
    $hash = (Get-FileHash -Path $setupFile -Algorithm SHA256).Hash.ToLower()
    $hash | Out-File -FilePath "$setupFile.sha256" -NoNewline -Encoding ascii
    Write-Host "    SHA-256 sidecar created: $setupFile.sha256"
    Write-Host "    Checksum: $hash"
} else {
    Write-Error "Installer executable was not found at: $setupFile"
}

Write-Host "==> Windows Installer successfully created at: $setupFile"
