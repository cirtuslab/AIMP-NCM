param([string]$Version="1.0.0")
$ErrorActionPreference="Stop"
Write-Host "=== AIMP NCM Release $Version ==="

# 1. Build plugin (x64+x86)
Write-Host "[1/3] Building plugin..."
& "$PSScriptRoot\build.ps1" 2>&1 | Out-Host
if($LASTEXITCODE -ne 0){ throw "build plugin failed" }

# 2. Build GUI EXE (optional, skip if pyinstaller not available)
Write-Host "[2/3] Building GUI EXE..."
try {
  & "$PSScriptRoot\gui\build_exe.ps1" 2>&1 | Out-Host
} catch {
  Write-Host "GUI EXE build skipped: $_"
}

# 3. Package release
Write-Host "[3/3] Packaging..."
$releaseDir = Join-Path $PSScriptRoot "release\AIMP_NCM_v$Version"
Remove-Item -Recurse -Force $releaseDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
# Plugin
Copy-Item -Force (Join-Path $PSScriptRoot "dist\aimp_ncm.aimppack") $releaseDir
Copy-Item -Force (Join-Path $PSScriptRoot "dist\README_INSTALL.txt") $releaseDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $releaseDir "manual") | Out-Null
Copy-Item -Force (Join-Path $PSScriptRoot "dist\aimp_ncm.dll") (Join-Path $releaseDir "manual") -ErrorAction SilentlyContinue
Copy-Item -Force (Join-Path $PSScriptRoot "dist\x86\aimp_ncm.dll") (Join-Path $releaseDir "manual\x86_aimp_ncm.dll") -ErrorAction SilentlyContinue
# GUI
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot "gui") (Join-Path $releaseDir "gui")
if(Test-Path (Join-Path $PSScriptRoot "dist_gui\AIMP_NCM_GUI.exe")){
  Copy-Item -Force (Join-Path $PSScriptRoot "dist_gui\AIMP_NCM_GUI.exe") $releaseDir
}
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
