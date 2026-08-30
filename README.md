# AIMP NCM · 网易云音乐串流插件

> **AIMPack 插件**（AIMP 5.x，x86 + x64）：在 AIMP 中直接播放你的网易云音乐歌单。
> 支持 Cookie 登录、八档音质切换、歌单一键同步；播放时本地代理把真实音频流交给 AIMP，
> 并在流内注入完整歌曲信息（标题 / 歌手 / 专辑 / 封面 / 可选歌词），音频落盘缓存、支持拖动进度。

![Language](https://img.shields.io/badge/language-C%2B%2B-00599C)
![Version](https://img.shields.io/github/v/release/cirtuslab/AIMP-NCM?label=version&color=blue)
![License](https://img.shields.io/badge/license-MIT-green)
![AIMP](https://img.shields.io/badge/AIMP-5%2B-orange)

## 功能特性

- **原生设置页**：集成在 `设置 → 插件 → 网易云串流`，跟随 AIMP 皮肤
- **Cookie 直填登录**：粘贴 `MUSIC_U` 即登录（自动补键名、去引号与空白）；
  只粘裸值也可以
- **歌单同步**：勾选歌单 → 点「应用」→ 自动创建/复用播放列表「网易云串流」（同名不重复创建），
  勾选状态持久化；超过 1000 首的大歌单自动分页拉取
- **完整歌曲信息流内注入**：播放时向音频流前置 ID3v2.3 标签（标题 / 歌手 / 专辑 / 封面 APIC），
  AIMP 播放器与封面面板直接显示；封面按播放即时下载并磁盘缓存，**无批量请求**
- **歌词注入（可选）**：USLT 帧内嵌歌词，原词 + 翻译按时间戳就近合并（±350ms），
  mp3 / flac / wav 均验证可用，设置页可开关
- **本地代理播放**：内嵌 `http://127.0.0.1:{port}` 服务（默认 47777，占用自动后移），
  实时解析真实 CDN 链接并代理拉流，支持磁盘缓存与 Range/seek——链接不过期、无需预取；
  下载失败自动重试（同一链接最多 3 次），仍失败弹窗提示「此曲不可用」
- **音质八档**：`standard / higher / exhigh / lossless / hires / jymaster / jyeffect / sky`，
  配置音质不存在时自动降到可用最高档（高音质需会员）；切换后**下次播放起生效**
- **磁盘缓存策略**：保留天数可选（1/3/7/30 天或永不删除），支持白名单歌单缓存永不清理
- 直连国内可用；海外/受限网络可启用代理镜像（Node 或 Python 实现）
- 401/402/403/429/451 等访问受限统一弹窗提示（60s 冷却防刷屏）

## 安装

从 [Releases](https://github.com/cirtuslab/AIMP-NCM/releases/latest) 下载 `aimp_ncm.aimppack`：

- 双击 `aimp_ncm.aimppack` 即可完成安装（可能弹 UAC）
- 或在 AIMP 内安装：主菜单 → 选项... → 插件 → 安装，选择 `.aimppack`，重启 AIMP 生效

> 注：旧版曾提供 `tools/fix_aimp_plugin_perm.ps1` 免 UAC 安装脚本（给 `AIMP\Plugins`
> 目录授 Users 写权限）。该做法会降低系统目录 ACL，允许普通进程向插件目录写入 DLL，
> 属于安全风险，已移除；请直接使用 UAC 安装或手动复制 DLL。

从源码构建（需要 VS2022/2026，含 C++ 工具链与 CMake）：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1     # x86+x64 构建, 生成 dist/aimp_ncm.aimppack
powershell -ExecutionPolicy Bypass -File release.ps1   # 生成 Release 压缩包
```

## 使用（四步）

1. **登录**：先在浏览器登录 music.163.com，然后把 Cookie 粘贴进设置页
   （F12 → 应用/存储 → Cookie → 复制 `MUSIC_U` 整段；值长达数百字符属正常）
2. **网络**：国内保持直连；海外勾选「启用代理」并填镜像地址
   （如 `iwenwiki.com:3000`，带不带 `http://` 都行），点「测试」验证
3. **拉取歌单**：点「刷新歌单」（UID 缺失会自动补全）
4. **同步**：勾选歌单 → 点「应用」→ 左侧出现「网易云串流」播放列表，双击即播

## 配置文件

配置目录为 `%APPDATA%\AIMP\NcmPlugin\`，除 `config.json` 外，同步生成的
`song_meta.json`（歌单元数据）与 `ncm_playlist.m3u8`（播放列表交接文件）也放在这里。

`config.json` 字段：

| 字段 | 含义 |
|---|---|
| `cookieEnc` | 登录态（`MUSIC_U=...` 等），**DPAPI 加密**保存（base64）；旧版明文 `cookie` 字段自动迁移 |
| `apiUrl` | 镜像地址，为空则直连 `music.163.com` |
| `mirrorTokenEnc` | 镜像共享 Token（可选，DPAPI 加密保存；经 `X-NCM-Token` 头发送） |
| `useProxy` | `true` 走镜像，`false` 直连 |
| `quality` | 音质档位，默认 `exhigh` |
| `uid` | 登录账号 ID，拉歌单时自动补全 |
| `localPort` | 本地代理端口，默认 47777，被占用时自动 +1 并回写 |
| `cacheDays` | 播放缓存保留天数；`<=0` 永不删除（白名单例外） |
| `cacheWhitelist` | 白名单歌单 ID（逗号分隔），其缓存永不清理 |
| `lyricMode` | 歌词注入：`none` 不注入 / `uslt` 流内注入（默认） |
| `deviceCookie` | 设备指纹 Cookie（首次生成后持久化，仅登录/换 Cookie 时刷新） |
| `localTokenEnc` | 本地代理访问 Token（随机生成，DPAPI 加密保存；播放列表 URL 携带） |
| `selectedPlaylists` | 勾选待同步的歌单 ID 数组 |

> 凭据字段（`cookieEnc`/`mirrorTokenEnc`/`localTokenEnc`）使用 Windows DPAPI 加密，
> 仅当前 Windows 用户可解密；请勿把 `config.json` 复制到其他机器/用户。
> 若旧版明文配置存在，首次保存时会自动加密迁移。

## 本地缓存与日志

播放产生的临时数据都在系统临时目录 `%TEMP%\aimp_ncm\` 下（可随时整目录清理，
元数据与配置在 `%APPDATA%\AIMP\NcmPlugin\` 不受影响）：

| 路径 | 内容 |
|---|---|
| `cache\{pid}_{tid}.{ext}` | 音频流缓存（未完成的下载为 `.part` 临时文件，不会被当作缓存命中） |
| `artwork\{tid}.img` | 封面缓存（首次播放该曲时下载） |
| `lyric\{tid}.lrc` | 歌词缓存 |
| `logs\aimp_ncm_fs.log` | 诊断日志（单文件 3MB 上限，滚动保留 7 天） |

## 服务端镜像（可选）

海外/受限网络时的中转，两种实现任选其一（默认端口 3000，均**仅监听 127.0.0.1**）：

```bash
cd ncm_service && npm install && node server.js
# 或 Python:
pip install -r ncm_service/requirements.txt && python ncm_service/app.py
```

镜像服务按设计接受 `cookie` 参数，因此只应在本机/可信内网使用，勿暴露公网。

若需部署到公网/多机使用，建议开启共享 Token 鉴权（插件设置页「镜像Token」对应）：

```bash
NCM_MIRROR_TOKEN=你的随机密钥 node server.js        # Node
# 或
NCM_MIRROR_TOKEN=你的随机密钥 python app.py          # Python
```

开启后所有请求必须携带 `X-NCM-Token` 头（插件自动发送），否则返回 403。
镜像服务同时只开放插件用到的接口白名单，其余路径一律 404；部署公网请务必加 TLS 反代。

## 常见问题

- **播放器不显示歌曲信息/封面？** 重新点一次「应用」同步歌单（元数据在同步时写入 song_meta.json）；
  封面在首次播放该曲时即时下载，失败会自动跳过、不影响播放
- **灰色/404？** 会员音质或版权限制；取链会自动降到可用最高音质，
  拉流失败不降级，重试 3 次仍失败会弹窗提示「此曲不可用」
- **境外 460/403？** 启用代理并填国内 VPS 镜像
- **「应用」按钮是灰的？** 先「刷新歌单」，勾选后按钮点亮
- **提示 401/403/429？** Cookie 可能已失效或触发风控：重新粘贴 `MUSIC_U`，
  或稍后重试 / 更换镜像
- **中文用户名 Windows 账户能用吗？** 能。v1.5.1 起配置与元数据读写全部改用宽字符路径，
  不再依赖系统 ANSI 代码页
- **音质切换后没变化？** 音质在下次播放起生效；播放中的曲目保持当前流

## 参考的开源项目

本项目的协议实现与 AIMP 集成方式大量借鉴了以下开源项目，特此致谢：

| 项目 | 主要参考点 |
|------|-----------|
| [Binaryify/NeteaseCloudMusicApi](https://github.com/Binaryify/NeteaseCloudMusicApi) | Node 镜像服务基础；weapi/eapi 接口行为基准 |
| [NeteaseCloudMusicApiEnhanced/api-enhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced) | 设备指纹/Cookie 对象处理参考 |
| [go-musicfox/go-musicfox](https://github.com/go-musicfox/go-musicfox) | 设备指纹 Cookie（`sDeviceId`/`_ntes_nuid` 等）与 eapi 歌曲链接参数构造 |
| [chaunsin/netease-cloud-music](https://github.com/chaunsin/netease-cloud-music) | 网易云协议逆向研究资料（eapi 加密细节） |
| [AdrianEddy/AIMPYouTube](https://github.com/AdrianEddy/AIMPYouTube) | AIMP 文件系统扩展注册方式与虚拟流播放模式 |
| [cirtuslab/aimp_desktop_lyrics](https://github.com/cirtuslab/aimp_desktop_lyrics) | AIMP 原生选项页控件布局范式；插件目录权限免 UAC 处理 |
| [cirtuslab/AIMPLyricsSaver](https://github.com/cirtuslab/AIMPLyricsSaver) | WinHTTP + RapidJSON 的 C++ 插件实现范式；歌词翻译合并策略 |
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
