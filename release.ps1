param([string]$Version="1.5.1")
$ErrorActionPreference="Stop"
Write-Host "=== AIMP NCM Release $Version ==="

# 1. Build plugin (x64+x86)
Write-Host "[1/2] Building plugin..."
& "$PSScriptRoot\build.ps1" 2>&1 | Out-Host
if($LASTEXITCODE -ne 0){ throw "build plugin failed" }

# 2. Package release
Write-Host "[2/2] Packaging..."
$releaseDir = Join-Path $PSScriptRoot "release\AIMP_NCM_v$Version"
Remove-Item -Recurse -Force $releaseDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
# Plugin
Copy-Item -Force (Join-Path $PSScriptRoot "dist\aimp_ncm.aimppack") $releaseDir
Copy-Item -Force (Join-Path $PSScriptRoot "dist\README_INSTALL.txt") $releaseDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $releaseDir "manual") | Out-Null
Copy-Item -Force (Join-Path $PSScriptRoot "dist\aimp_ncm.dll") (Join-Path $releaseDir "manual") -ErrorAction SilentlyContinue
Copy-Item -Force (Join-Path $PSScriptRoot "dist\x86\aimp_ncm.dll") (Join-Path $releaseDir "manual\x86_aimp_ncm.dll") -ErrorAction SilentlyContinue
# Service
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot "ncm_service") (Join-Path $releaseDir "ncm_service")
# Docs
Copy-Item -Force (Join-Path $PSScriptRoot "README.md") $releaseDir

$zip = Join-Path $PSScriptRoot "release\AIMP_NCM_v$Version.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $releaseDir -DestinationPath $zip -Force
Write-Host "Release created: $zip"
Get-ChildItem $releaseDir -Recurse | Format-Table FullName
Get-ChildItem (Split-Path $zip) | Format-Table Name,Length
Write-Host "Done. Deliver: $zip"
