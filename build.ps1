param(
  [string]$Config="Release"
)
$ErrorActionPreference="Stop"

$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if(Test-Path $vsWhere){
  $vsPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
}
if(!$vsPath -or !(Test-Path $vsPath)){ $vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools" }
if(!(Test-Path $vsPath)){ $vsPath="C:\Program Files\Microsoft Visual Studio\2022\Community" }
Write-Host "VS Path: $vsPath"

# 优先使用 VS 自带的 cmake（与 MSVC 工具链匹配），其次 PATH，最后下载便携版
$cmakeVs = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$cmakeExe = $null
if(Test-Path $cmakeVs){ $cmakeExe = $cmakeVs }
if(!$cmakeExe){
  $cmd = Get-Command cmake -ErrorAction SilentlyContinue
  if($cmd){ $cmakeExe = $cmd.Source; if(!$cmakeExe){ $cmakeExe = $cmd.Path } }
}
if(!$cmakeExe){
  Write-Host "Downloading portable cmake..."
  $tmp = Join-Path $env:TEMP "opencode\cmake"
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  $zip="$tmp\cmake.zip"
  Invoke-WebRequest -Uri "https://github.com/Kitware/CMake/releases/download/v3.28.0/cmake-3.28.0-windows-x86_64.zip" -OutFile $zip -UseBasicParsing
  Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
  $cmakeExe = "$tmp\cmake-3.28.0-windows-x86_64\bin\cmake.exe"
}
Write-Host "CMake: $cmakeExe"

# 显式指定 VS 生成器（默认生成器在部分环境会找不到 MSVC 工具链）
$genArg = @()
if($vsPath -match '2022'){ $genArg = @('-G','Visual Studio 17 2022') }
elseif($vsPath -match '2019'){ $genArg = @('-G','Visual Studio 16 2019') }

# Build x64
$buildDir = Join-Path $PSScriptRoot "build"
Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $buildDir | Out-Null
& $cmakeExe -S $PSScriptRoot -B $buildDir @genArg -A x64 2>&1 | Out-Host
if($LASTEXITCODE -ne 0){ throw "cmake x64 configure failed" }
& $cmakeExe --build $buildDir --config $Config --parallel 2>&1 | Out-Host
if($LASTEXITCODE -ne 0){ throw "build x64 failed" }

# Build x86
$buildDirX86 = Join-Path $PSScriptRoot "build_x86"
Remove-Item -LiteralPath $buildDirX86 -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $buildDirX86 | Out-Null
& $cmakeExe -S $PSScriptRoot -B $buildDirX86 @genArg -A Win32 2>&1 | Out-Host
if($LASTEXITCODE -ne 0){ throw "cmake x86 configure failed" }
& $cmakeExe --build $buildDirX86 --config $Config --parallel 2>&1 | Out-Host
if($LASTEXITCODE -ne 0){ throw "build x86 failed" }

# Package (correct layout: aimp_ncm\aimp_ncm.dll (x86) + aimp_ncm\x64\aimp_ncm.dll)
$stage = Join-Path $buildDir "aimppack_stage\aimp_ncm"
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$stage\x64" | Out-Null
Copy-Item -Force (Join-Path $buildDir "plugin\$Config\aimp_ncm.dll") "$stage\x64\aimp_ncm.dll"
# Fallback path
if(!(Test-Path "$stage\x64\aimp_ncm.dll")){
  Copy-Item -Force (Join-Path $buildDir "plugin\aimp_ncm.dll") "$stage\x64\aimp_ncm.dll" -ErrorAction SilentlyContinue
}
Copy-Item -Force (Join-Path $buildDirX86 "plugin\$Config\aimp_ncm.dll") "$stage\aimp_ncm.dll"
if(!(Test-Path "$stage\aimp_ncm.dll")){
  Copy-Item -Force (Join-Path $buildDirX86 "plugin\aimp_ncm.dll") "$stage\aimp_ncm.dll" -ErrorAction SilentlyContinue
}
Copy-Item -Force (Join-Path $PSScriptRoot "packaging\ReadMe.txt") $stage -ErrorAction SilentlyContinue
Copy-Item -Force (Join-Path $PSScriptRoot "packaging\manifest.xml") (Join-Path $stage "aimp_ncm.dll.manifest") -ErrorAction SilentlyContinue
Copy-Item -Force (Join-Path $PSScriptRoot "packaging\manifest.xml") (Join-Path $stage "x64\aimp_ncm.dll.manifest") -ErrorAction SilentlyContinue
Set-Content -LiteralPath (Join-Path $stage "LICENSE") -Value "MIT" -Encoding UTF8 -ErrorAction SilentlyContinue

$zipTmp = Join-Path $buildDir "AIMP_NCM.zip"
Remove-Item -Force $zipTmp -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zipTmp -Force

$dst = Join-Path $PSScriptRoot "dist"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item -Force $zipTmp (Join-Path $dst "aimp_ncm.aimppack")
Copy-Item -Force (Join-Path $stage "x64\aimp_ncm.dll") (Join-Path $dst "aimp_ncm.dll")
New-Item -ItemType Directory -Force -Path (Join-Path $dst "x86") | Out-Null
Copy-Item -Force (Join-Path $stage "aimp_ncm.dll") (Join-Path $dst "x86\aimp_ncm.dll")

Write-Host "Build OK:"
Get-ChildItem $dst | Format-Table Name,Length
Write-Host "已生成 AIMP 规范安装包: aimp_ncm/aimp_ncm.dll (x86) + aimp_ncm/x64/aimp_ncm.dll"
Write-Host "若 aimppack 提示 invalid，请手动将 dist\aimp_ncm.dll 与 dist\x86\aimp_ncm.dll 放入 AIMP\Plugins 对应目录"

