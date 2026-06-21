# PowerShell script to build the Windows installer for CUE SAMPLER.
# Generates the installer executable and a SHA-256 checksum sidecar file.
#
# Usage: .\make-installer-windows.ps1 -Version "1.0.0" -BuildDir "build"

param (
    [string]$Version = "1.0.0",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

Write-Host "==> Preparing Windows Installer (Version: $Version, BuildDir: $BuildDir)..."

# --- 1. Generate LICENSE.txt from EULA.md ---
if (Test-Path "EULA.md") {
    Write-Host "==> Generating LICENSE.txt from EULA.md..."
    $eula = Get-Content -Path "EULA.md" -Raw
    # Strip basic markdown headers and bolding
    $license = $eula -replace '(?m)^#\s+', '' -replace '(?m)^##\s+', '' -replace '\*\*', ''
    $license | Out-File -FilePath "LICENSE.txt" -Encoding utf8
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

# --- 3. Locate Inno Setup Compiler (ISCC.exe) ---
$iscc = "ISCC.exe"
$commonPaths = @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)

$found = $false
foreach ($path in $commonPaths) {
    if (Test-Path $path) {
        $iscc = $path
        $found = $true
        break
    }
}

if (-not $found) {
    # Check if ISCC is in the environment PATH
    $null = Get-Command $iscc -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -eq 0) {
        $found = $true
    }
}

if (-not $found) {
    Write-Error "Inno Setup Compiler (ISCC.exe) was not found in standard paths or in the PATH env variable. Please install Inno Setup 6."
}

Write-Host "==> Using Inno Setup Compiler: $iscc"

# --- 4. Create output folder ---
if (-not (Test-Path "dist")) {
    New-Item -ItemType Directory -Path "dist" | Out-Null
}

# --- 5. Compile the Installer ---
Write-Host "==> Compiling installer..."
& $iscc /dMyAppVersion=$Version installer.iss

# --- 6. Generate SHA-256 Checksum Sidecar for Updater ---
$setupFile = "dist\CUESAMPLER-Setup-$Version.exe"
if (Test-Path $setupFile) {
    Write-Host "==> Generating SHA-256 checksum sidecar..."
    $hash = (Get-FileHash -Path $setupFile -Algorithm SHA256).Hash.ToLower()
    $hash | Out-File -FilePath "$setupFile.sha256" -NoNewline -Encoding ascii
    Write-Host "    SHA-256 sidecar created: $setupFile.sha256"
    Write-Host "    Checksum: $hash"
} else {
    Write-Error "Installer executable was not found at: $setupFile"
}

Write-Host "==> Windows Installer successfully created at: $setupFile"
