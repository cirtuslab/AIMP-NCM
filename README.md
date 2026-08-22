# AIMP NCM · 网易云音乐串流插件

> **AIMPack 插件**（AIMP 5.40+，x86 + x64）：在 AIMP 中直接播放你的网易云音乐歌单。
> 支持扫码 / 网页 Cookie 登录、音质切换、歌单一键同步，播放时显示完整歌曲信息（标题 / 歌手 / 专辑 / 封面）。

![GitHub Release](https://img.shields.io/github/v/release/cirtuslab/AIMP-NCM)

## 功能特性

- **原生设置页**：集成在 `设置 → 插件 → 网易云串流`，跟随 AIMP 皮肤
- **登录三通道**：
  1. 🌐 **官方网页登录（推荐）**——在浏览器正常登录 music.163.com 后粘贴 Cookie，无风控（GUI 另支持从 Edge/Firefox/Chrome 自动读取）
  2. 📱 **扫码登录**——新协议（`type=3` + `chainId`）；若命中滑块验证会自动用浏览器打开验证页，完成后自动继续
  3. 📋 **Cookie 直填**——只粘裸值也可以（自动补 `MUSIC_U=` 键名）
- **歌单同步**：勾选歌单 → 点「应用」→ 自动创建/刷新播放列表「网易云串流」，勾选状态持久化
- **完整歌曲信息**：播放时本地代理向音频流注入 ID3v2 标签（标题 / 歌手 / 专辑 / 时长 / 封面），AIMP 播放器与封面面板直接显示；封面按播放即时下载并磁盘缓存，**无批量请求**
- **本地代理播放**：内嵌 `http://127.0.0.1:{port}` 服务（默认 47777，占用自动后移），实时解析真实 CDN 链接并代理拉流，支持磁盘缓存与 Range/seek——链接不过期、无需预取
- **音质**：`standard / higher / exhigh / lossless / hires / jymaster / jyeffect / sky`，实时生效（高音质需会员，否则自动降级）
- 直连国内可用；海外/受限网络可启用代理镜像

## 安装

从 [Releases](https://github.com/cirtuslab/AIMP-NCM/releases/latest) 下载 `aimp_ncm.aimppack`：

```powershell
# 方式一：双击 aimp_ncm.aimppack（可能弹 UAC）
# 方式二：静默安装
powershell -ExecutionPolicy Bypass -File install.ps1
# 免 UAC（管理员运行一次即可）
powershell -ExecutionPolicy Bypass -File tools/fix_aimp_plugin_perm.ps1
```

从源码构建：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1   # 生成 dist/aimp_ncm.aimppack (x86+x64)
```

需要 VS2022/2026（含 C++ 工具链与 CMake）。

## 使用（四步）

1. **登录**：推荐先在浏览器登录 music.163.com，然后把 Cookie 粘贴进设置页
   （F12 → 应用/存储 → Cookie → 复制 `MUSIC_U` 整段；值长达数百字符属正常）
2. **网络**：国内保持直连；海外勾选「启用代理」并填镜像地址（如 `iwenwiki.com:3000`，带不带 `http://` 都行）
3. **拉取歌单**：点「刷新歌单」（UID 缺失会自动补全）
4. **同步**：勾选歌单 → 点「应用」→ 左侧出现「网易云串流」播放列表，双击即播

## 配置文件

`%APPDATA%\AIMP\NcmPlugin\config.json`（插件与 GUI 共用）

```json
{
  "cookie": "MUSIC_U=...",
  "apiUrl": "",
  "useProxy": false,
  "quality": "exhigh",
  "uid": "123456",
  "localPort": 47777,
  "selectedPlaylists": [123456789]
}
```

## 架构

```
AIMP 播放 http://127.0.0.1:{port}/{pid}/{tid}.mp3
        └─► 插件内嵌 LocalServer: eapi/weapi 实时解析真实 CDN 链接
             ├─► 缓存命中 → 直接回本地缓存文件（支持 Range）
             └─► 未命中 → 代理拉流并落盘缓存
             响应前置 ID3v2 标签(标题/歌手/专辑/时长/封面)
                  └─► AIMP 解码器解析标签 → 播放器显示歌曲信息与封面

设置页 ──► weapi/eapi 直连 music.163.com 或 镜像 http://{apiUrl}
登录   ──► 扫码: /weapi/login/qrcode/* (+chainId 防风控)
        └─► Cookie 直填 / 浏览器导入(GUI)
```

## GUI 调试工具（可选，非必需）

`gui/app.py` 为 Tkinter 控制中心，定位是**测试/调试**：

- 三种登录方式测试（扫码 / 从浏览器导入 / 粘贴 Cookie），日志可视化
- 镜像连通性测试、直连 weapi 自检
- 歌单拉取、批量预取真实链接生成 m3u8（不依赖插件的备用方案）
- 与插件共用同一份 config.json 与歌曲元数据缓存，改完即生效

```bash
pip install -r gui/requirements.txt
python gui/app.py
# 打包单目录 EXE（不易被杀软误报）:
powershell -ExecutionPolicy Bypass -File gui/build_exe.ps1
```

## 服务端镜像（可选）

海外/受限网络时的中转：

```bash
cd ncm_service && npm install && node server.js     # Node: 基于 NeteaseCloudMusicApi
# 或 Python: pip install requests pycryptodome && python ncm_service/app.py
```

## 常见问题

- **播放器不显示歌曲信息/封面？** 重新点一次「应用」同步歌单（元数据在同步时写入本地缓存）；封面在首次播放该曲时即时下载，失败会自动跳过、不影响播放
- **灰色/404？** 会员音质或版权限制，插件自动降级到 exhigh
- **境外 460/403？** 启用代理并填国内 VPS 镜像
- **扫码提示环境异常/滑块？** 会自动打开验证页，完成后继续；或改用官方网页 Cookie 方式（推荐）
- **应用按钮灰的？** 先「刷新歌单」，勾选后按钮点亮
- **杀软报 GUI EXE？** PyInstaller 通病，用 onedir 模式产物或加白名单

## 参考的开源项目

本项目的协议实现与 AIMP 集成方式大量借鉴了以下开源项目，特此致谢：

| 项目 | 主要参考点 |
|------|-----------|
| [Binaryify/NeteaseCloudMusicApi](https://github.com/Binaryify/NeteaseCloudMusicApi) | Node 镜像服务基础；weapi/eapi 接口行为基准 |
| [NeteaseCloudMusicApiEnhanced/api-enhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced) | 扫码登录新协议（`type=3` + `chainId`）与滑块风控对策 |
| [go-musicfox/go-musicfox](https://github.com/go-musicfox/go-musicfox) | 设备指纹 Cookie（`sDeviceId`/`_ntes_nuid` 等）与 eapi 歌曲链接参数构造 |
| [chaunsin/netease-cloud-music](https://github.com/chaunsin/netease-cloud-music) | 网易云协议逆向研究资料（eapi 加密细节） |
| [AdrianEddy/AIMPYouTube](https://github.com/AdrianEddy/AIMPYouTube) | AIMP 文件系统扩展注册方式与虚拟流播放模式 |
| [cirtuslab/aimp_desktop_lyrics](https://github.com/cirtuslab/aimp_desktop_lyrics) | AIMP 原生选项页控件布局范式；插件目录权限免 UAC 处理 |
| [cirtuslab/AIMPLyricsSaver](https://github.com/cirtuslab/AIMPLyricsSaver) | WinHTTP + RapidJSON 的 C++ 插件实现范式 |
| [cirtuslab/SongMetaFixer](https://github.com/cirtuslab/SongMetaFixer) | 歌曲元数据字段集与网易云详情接口数据源 |
| [imsyy/SPlayer](https://github.com/imsyy/SPlayer) | 「官方网页登录 → 读取 Cookie」的免风控登录流程 |
| [martin211/aimp_dotnet](https://github.com/martin211/aimp_dotnet) | AIMP 各扩展类别注册 IID 映射的权威佐证 |

## 许可

MIT。第三方：AIMP SDK © Artem Izmaylov，nlohmann/json MIT，NeteaseCloudMusicApi MIT。

---

## ⚠️ 使用风险提示

1. **本项目仅供个人学习与研究**，请勿用于任何商业用途。
2. 通过非官方接口访问网易云音乐服务**可能违反《网易云音乐用户协议》**，由此导致的账号风控、限流、封禁等后果由使用者自行承担。
3. 登录凭据（MUSIC_U Cookie 等）以**明文**保存在本地 `%APPDATA%\AIMP\NcmPlugin\config.json` 中，请妥善保管，切勿分享给他人或上传至公开场合；Cookie 泄露等同账号泄露。
4. 插件会在本机回环地址（127.0.0.1）开启一个 HTTP 端口用于播放代理，仅本机可访问；请勿将其暴露到公网。
5. 网易云音乐接口随时可能变更导致本项目部分或全部功能失效，作者不承诺持续维护。
6. 本项目与网易云音乐（网易公司）官方**无任何关联**，不代表官方立场；相关版权归原权利人所有。
7. **下载、安装或使用本软件即表示您已阅读并理解上述风险，并自愿承担全部责任。** 如不同意，请立即停止使用并卸载。
