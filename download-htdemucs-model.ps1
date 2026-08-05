# Downloads the pinned single-file HTDemucs fp32 ONNX model used by
# StemSeparator and verifies the publisher's Git LFS SHA-256 before installing
# it into the ignored assets directory.

param (
    [string]$Destination = "assets\htdemucs\htdemucs.onnx"
)

$ErrorActionPreference = "Stop"

$modelUrl = "https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/main/htdemucs.onnx"
$expectedSha256 = "68d0bf16428ef66e692cdff8a9ccf28f1ef3f69440d57e58605a4cc55fcc5e74"
$destinationPath = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Destination))
$destinationDir = Split-Path -Parent $destinationPath
$downloadPath = "$destinationPath.download"

New-Item -ItemType Directory -Path $destinationDir -Force | Out-Null

if (Test-Path $destinationPath -PathType Leaf) {
    $existingHash = (Get-FileHash -Path $destinationPath -Algorithm SHA256).Hash.ToLower()
    if ($existingHash -eq $expectedSha256) {
        Write-Host "HTDemucs is already present and verified: $destinationPath"
        exit 0
    }
    Write-Warning "The existing model does not match the pinned release; downloading a verified replacement."
}

if (Test-Path $downloadPath -PathType Leaf) {
    Remove-Item -LiteralPath $downloadPath -Force
}

Write-Host "Downloading HTDemucs (316,446,953 bytes)..."
& curl.exe -L --fail --retry 3 --output $downloadPath $modelUrl
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $downloadPath -PathType Leaf)) {
    Write-Error "HTDemucs download failed."
}

$downloadHash = (Get-FileHash -Path $downloadPath -Algorithm SHA256).Hash.ToLower()
if ($downloadHash -ne $expectedSha256) {
    Remove-Item -LiteralPath $downloadPath -Force
    Write-Error "HTDemucs integrity check failed. Expected $expectedSha256, got $downloadHash."
}

Move-Item -LiteralPath $downloadPath -Destination $destinationPath -Force
Write-Host "HTDemucs installed and verified: $destinationPath"
Write-Host "SHA-256: $downloadHash"
