# 关于 AIMP 插件安装权限的说明
#
# 历史版本曾提供"给 C:\Program Files\AIMP\Plugins 授予 Users 写权限"的脚本,
# 以省去安装 aimppack 时的 UAC 提权。该做法会降低系统目录 ACL, 使普通进程
# 可向插件目录写入 DLL, 属于安全风险, 已移除。
#
# 正确安装方式(无需改任何目录权限):
#   1. 双击 aimp_ncm.aimppack, 在弹出的 UAC 提示中点"是"即可安装;
#      或
#   2. 手动将 dist\x86\aimp_ncm.dll 复制到 AIMP\Plugins\,
#      dist\aimp_ncm.dll(x64) 复制到 AIMP\Plugins\x64\, 重启 AIMP。
#
# 若曾运行过旧版授权脚本, 建议恢复 Plugins 目录默认 ACL(管理员 PowerShell):
#   icacls "C:\Program Files\AIMP\Plugins" /reset /T
