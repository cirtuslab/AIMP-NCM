param(
  [string]$Python="python",
  [switch]$OneFile   # default: onedir (less AV false positives); use -OneFile for single exe
)
$ErrorActionPreference="Stop"
Write-Host "Building GUI EXE via PyInstaller..."
& $Python -m pip install -q pyinstaller requests qrcode Pillow pycryptodome
if($LASTEXITCODE -ne 0){ throw "pip install failed" }

$root = $PSScriptRoot | Split-Path  # project root (script lives in gui/)
Set-Location $root

# onedir avoids runtime self-extraction which triggers AV heuristics;
# embedding a version resource also lowers false-positive rate
$iconArg = ""
if(Test-Path "$root\gui\icon.ico"){ $iconArg = "--icon=`"$root\gui\icon.ico`"" }
$verArg = ""
if(Test-Path "$root\gui\version_info.txt"){ $verArg = "--version-file=`"$root\gui\version_info.txt`"" }

$modeArgs = @("--onedir")
if($OneFile){ $modeArgs = @("--onefile") }

$cmd = "& `"$Python`" -m PyInstaller --noconfirm --clean $modeArgs --windowed $iconArg $verArg " +
       "--exclude-module numpy --exclude-module matplotlib --exclude-module scipy " +
       "--name AIMP_NCM_GUI gui/app.py --distpath dist_gui --workpath build_gui --specpath build_gui"
Write-Host $cmd
Invoke-Expression $cmd
if($LASTEXITCODE -ne 0){ throw "pyinstaller failed" }

if($OneFile){
  Write-Host "Built dist_gui/AIMP_NCM_GUI.exe"
} else {
  Write-Host "Built dist_gui\AIMP_NCM_GUI\AIMP_NCM_GUI.exe  (ship the whole AIMP_NCM_GUI folder)"
}
Get-ChildItem dist_gui | Format-Table Name,Length
