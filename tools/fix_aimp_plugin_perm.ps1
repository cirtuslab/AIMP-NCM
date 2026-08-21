# 一次性修复: 允许普通用户安装 AIMP 插件而不触发 UAC
# 原理: 给 C:\Program Files\AIMP\Plugins 授予 Users 写权限,
#       之后 AIMP 安装 aimppack 无需再提权, 不再弹 UAC
# 用法: 右键"以管理员身份运行" 或:
#   powershell -ExecutionPolicy Bypass -File fix_aimp_plugin_perm.ps1

$ErrorActionPreference = "Stop"

# 检查是否管理员
$id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$p = New-Object System.Security.Principal.WindowsPrincipal($id)
if(-not $p.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)){
    Write-Host "需要管理员权限! 请右键此脚本 -> 以管理员身份运行" -ForegroundColor Red
    pause
    exit 1
}

# 定位 AIMP
$aimpDir = $null
$candidates = @(
    "C:\Program Files\AIMP",
    "C:\Program Files (x86)\AIMP",
    "$env:ProgramFiles\AIMP",
    "${env:ProgramFiles(x86)}\AIMP"
)
foreach($d in $candidates){
    if(Test-Path (Join-Path $d "AIMP.exe")){ $aimpDir = $d; break }
}
if(-not $aimpDir){
    Write-Host "未找到 AIMP 安装目录" -ForegroundColor Red
    pause
    exit 1
}
Write-Host "AIMP 目录: $aimpDir"

$pluginsDir = Join-Path $aimpDir "Plugins"
if(-not (Test-Path $pluginsDir)){
    Write-Host "未找到 Plugins 目录: $pluginsDir" -ForegroundColor Red
    pause
    exit 1
}

# 授予 Users 组: 修改+写入 (M) 权限, 继承到子目录
Write-Host "正在为 Users 组添加写权限到: $pluginsDir"
icacls $pluginsDir /grant "*S-1-5-32-545:(OI)(CI)M" /T
if($LASTEXITCODE -ne 0){
    Write-Host "授权失败" -ForegroundColor Red
    pause
    exit 1
}

Write-Host ""
Write-Host "完成! 现在双击 aimppack 安装插件不会再弹 UAC。" -ForegroundColor Green
Write-Host "（如果之前已弹 UAC 安装过旧插件, 建议先卸载旧插件再重装）"
pause
