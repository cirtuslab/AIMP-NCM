# AIMP-NCM 插件工作原理

> 本文档基于当前仓库源码逐行核对编写，描述**当前插件实际**的工作方式（当前版本 **v1.5**）。
> 旧版 README 与代码不一致之处的修正记录见第 11 节；代码层面尚未解决的问题见
> `问题.local.md`（本地文档，不入库）。

## 1. 一句话概括

AIMP 播放列表里的条目是 `http://127.0.0.1:{port}/{pid}/{tid}` 这种**本地代理 URL**
（无文件后缀：后缀仅为路由占位，实际容器以播放时响应为准）。
播放时，插件内嵌的 HTTP 服务（LocalServer）实时向网易云解析真实 CDN 链接，把音频流代理给 AIMP，
并在流开头**注入 ID3v2.3 标签**（标题/歌手/专辑/时长/封面 APIC/歌词 USLT），使 AIMP 的播放器界面、
封面面板直接显示歌曲信息；同时把音频落盘到临时目录做缓存，支持 Range/seek。

## 2. 组件总览

| 目录/文件 | 角色 |
|---|---|
| `plugin/` | C++ DLL `aimp_ncm.dll`，真正的 AIMP 插件（x86 + x64） |
| `ncm_service/` | 可选镜像服务：`server.js`（Node，封装 NeteaseCloudMusicApi）、`app.py`（Python 简化版） |
| `tools/fix_aimp_plugin_perm.ps1` | 免 UAC：给 AIMP 插件目录授权 Users 写权限 |
| `build.ps1` / `release.ps1` | x86+x64 构建与 `aimp_ncm.aimppack` 打包 |

## 3. 配置

路径：`%APPDATA%\AIMP\NcmPlugin\config.json`（插件使用）。

| 字段 | 含义 |
|---|---|
| `cookie` | 登录态（`MUSIC_U=...` 等），明文保存 |
| `apiUrl` | 镜像地址，为空则直连 `music.163.com` |
| `quality` | `standard/higher/exhigh/lossless/hires/jymaster/jyeffect/sky`，默认 `exhigh` |
| `useProxy` | `true` 走镜像，`false` 直连 |
| `uid` | 登录账号 ID，拉歌单时自动补全 |
| `localPort` | 本地代理端口，默认 47777，被占用时自动 +1 并回写 |
| `cacheDays` | 播放缓存保留天数；`-1` 永不删除（白名单例外） |
| `cacheWhitelist` | 白名单歌单 ID（逗号分隔），其缓存永不清理 |
| `lyricMode` | 歌词注入模式：`none` 不注入 / `uslt` 流内注入 USLT 歌词帧（默认，mp3/flac/wav 均验证可行） |
| `deviceCookie` | 设备指纹 Cookie（首次生成后持久化，仅登录/换 Cookie 时刷新） |
| `selectedPlaylists` | 勾选待同步的歌单 ID 数组 |

## 4. 插件生命周期与注册

`AIMPPluginGetHeader` → `AimpNcmPlugin`（`IAIMPPlugin` + `IAIMPExternalSettingsDialog`）。
`Initialize` 依次：

1. 注册 `ncm://` 文件系统与文件信息提供者
   （`IID_IAIMPServiceFileSystems` / `IID_IAIMPServiceFileInfo`，参考 AIMPYouTube 的注册方式）；
2. 启动 LocalServer（仅绑定 127.0.0.1，默认 47777，最多向后试 20 个端口，成功后回写实际端口到配置）；
3. 注册原生选项页（`IID_IAIMPServiceOptionsDialog`，即 设置 → 插件 → 网易云串流）。

`Finalize`：先停 LocalServer，再注销/释放两个扩展。注册后的扩展对象由插件持有到 `Finalize`，
不会提前 `Release`（代码注释明确说明这是为了防止 AIMP 持有悬空指针）。

## 5. 歌单同步（设置页点「应用」）

`NcmOptionsFrame::StartSync`（后台线程）：

1. 读取 `selectedPlaylists`；
2. 逐个歌单调 `NcmClient::GetPlaylistDetail`（镜像：`GET /playlist/track/all?id=..&limit=1000`；
   直连：weapi `/api/v6/playlist/detail`，超过 1000 首按 1000/页分页补拉）；
3. `NcmMeta::WritePlaylist` 把整张歌单写入 `%TEMP%\aimp_ncm\song_meta.json`
   （结构 `{pid: {tid: {title, artist, album, durationMs, coverUrl}}}`）；
4. 生成合并 m3u8：`%TEMP%\aimp_ncm\ncm_playlist.m3u8`，
   每首一行 `#EXTINF` + `http://127.0.0.1:{port}/{pid}/{tid}`；
5. 主线程（`WM_USER+5`）经 `IAIMPServicePlaylistManager` 找到或创建播放列表「网易云串流」，
   `BeginUpdate → DeleteAll → Add(m3u8) → EndUpdate`。

同步阶段**不**批量取真实链接、**不**批量下载封面；只写元数据缓存，链接在播放时才解析
（因此「链接不过期」）。

## 6. 播放链路（核心）

AIMP 请求 `http://127.0.0.1:{port}/{pid}/{tid}` → `LocalServer::HandleClient`：

1. 解析 `pid/tid`，解析 `Range` 头（只支持 `bytes=N-` 形式）；
2. `NcmMeta::BuildStreamTag` 生成 ID3v2.3 标签（`TIT2/TPE1/TALB/APIC` + 可选 `USLT` 歌词帧，
   不注入 TLEN，时长由 AIMP 按流实测），
   封面来自 `%TEMP%\aimp_ncm\artwork\{tid}.img`，歌词来自 `%TEMP%\aimp_ncm\lyric\{tid}.lrc`，
   没有则即时下载后落盘（歌词受 `lyricMode` 控制，失败不影响播放）；
3. **缓存命中** → `ServeFile`：虚拟流 = ID3 标签 + 音频文件，Range 偏移减去标签长度映射到文件；
4. **缓存未命中** → 取链：按配置音质从高到低回退（`GetSongUrlLevel`，直连 eapi
   `/api/song/enhance/player/url/v1`，镜像 `GET /song/url/v1`），取第一个可用的
   **最高音质**（配置音质不存在时自动降级）；
   **拉流失败不降级**：同一链接后台重试最多 3 次，仍失败则弹窗提示
   「此曲不可用」并向 AIMP 返回 404；
5. `ProxyAndCache`：WinHTTP 拉 CDN 流，边转发给 AIMP（只发标签之后的音频）边写
   `%TEMP%\aimp_ncm\cache\{pid}_{tid}.part`，下载完整后改名 `{pid}_{tid}.{ext}`；
   客户端中途断开不中断下载，以便下次命中缓存。

注意：**不是 302 重定向**。`local_server.h` 注释里的「302 重定向返回」已过时；
当前实现是直接代理 + 流内注入标签（这也是 AIMP 能拿到歌曲信息的原因——网络流不会走
FileInfoProvider 回调）。

## 7. 元数据与封面

- 元数据缓存 `%TEMP%\aimp_ncm\song_meta.json`：插件在内存维护 `map<pid, map<tid, NcmSong>>`，
  每次读取前对比文件 mtime，外部写入后插件自动感知；写入用互斥锁保护。
- 封面缓存 `%TEMP%\aimp_ncm\artwork\{tid}.img`：播放到该曲才下载（URL 自动补 `https:`，
  **以源头原始尺寸下载**，不再强制 `?param=500y500`），失败跳过、不影响播放。
- 除流内 ID3 外，`ncm://` 条目另有 `IAIMPExtensionFileInfoProvider::GetFileInfo` 通道，
  设置 `AIMP_FILEINFO_PROPID_TITLE/ARTIST/ALBUM/DURATION/ALBUMART`；未命中缓存时
  会同步调 `GetSongDetail` 并 `Upsert` 回缓存。
- 歌词：`lyricMode=uslt`（默认）时播放链路经 `NcmClient::GetLyric`（`/lyric` 接口）拉取 LRC，
  以 USLT 帧注入流标签；接口返回多版本（lrc 原词 / tlyric 翻译 / klyric·yrc 逐字），
  实现上**优先合并翻译**（按时间戳就近匹配、±350ms 容差，参考 AIMPLyricsSaver），
  逐字歌词暂不使用；实测 AIMP 对 mp3/flac/wav 注入均正常显示歌词。

## 8. 登录与网络

- **Cookie 直填（唯一登录方式）**：设置页输入，自动补 `MUSIC_U=` 键名、去引号/空白。
  扫码登录已移除（上游二维码协议失效），浏览器导入随 GUI 一并移除。
- **直连模式**：weapi/eapi 加密（AES-CBC/ECB + RSA，密钥/IV 硬编码）+ 设备指纹 Cookie
  （`sDeviceId`/`_ntes_nuid`/`NMTID` 等）；设备指纹**持久化复用**，仅在登录/换 Cookie
  时刷新；`uid` 缺失时自动经 `/user/account` 补全。
- **镜像模式**：请求打到 `apiUrl`，cookie 以 `cookie` 表单/查询参数透传。

## 9. GUI（已移除）

原 `gui/app.py`（Tkinter 控制中心）已删除：扫码登录与浏览器导入随之上线风险/维护成本高，
插件本体只保留 Cookie 直填。

## 10. ncm_service 镜像

- `server.js`（Node + NeteaseCloudMusicApi，默认端口 3000，**仅绑定 127.0.0.1**）：
  转发 `user/playlist`、`playlist/track/all`、`song/url/v1`、`song/detail`、`lyric`、
  `user/account`；按设计接受 `cookie` 参数，因此不监听公网地址。
- `app.py`（Python 简化版）：实现 `user/playlist`、`playlist/track/all`、`song/url/v1`、
  `song/detail`、`lyric`、`user/account`，与 Node 版接口集一致（按需选其一即可）。

## 11. 历史修正记录（原 README 与代码不一致的解决）

以下为旧版 README 与代码不一致之处，已随文档更新修正：

1. **302 重定向**：旧 README 架构图说「实时解析真实 CDN 链接并 302 重定向」；
   实际是**直接代理转发 + 注入 ID3 标签**，代码里根本没有 302。
2. **登录通道**：旧 README 宣称「三通道登录」；现已改为仅 Cookie 直填，
   扫码（上游协议失效）与浏览器导入均已移除。
3. **音质「实时生效」**：旧 README 说音质实时生效；设置页 UI 自己的提示是
   「切换音质实时生效，**下次播放起生效**」，即只影响后续新请求。
4. **GUI 定位矛盾**：已随 GUI 移除解决（原 README 与 `gui/README.md` 互相矛盾）。
5. **`ncm://` 备用路径**：设置页同步写 `http://127.0.0.1` 本地代理条目；
   `ncm://` 文件系统仍注册，作为备用/兼容路径。
6. **配置示例不全**：旧 README 的 config.json 示例缺 `cacheDays` / `cacheWhitelist` /
   `lyricMode`（代码实际读写这些字段）。
7. **版本号不统一**：插件 `InfoGet` 自述 `[v1.5]`；README 徽章无对应版本说明（仍待统一）。
8. **遗留入口**：旧设置对话框（`ui_dialog.cpp`，资源 ID 101）曾与原生选项页并存，
   生成真实 CDN URL 的 m3u8（10 分钟过期）；已下线删除（B4）。

## 12. 已知边界

- 歌单超过 1000 首时按 1000/页分页拉取（直连经 trackIds + v3/song/detail 补拉，
  镜像经 `/playlist/track/all` 的 offset 分页）；m3u8 仍逐首写入，大歌单较慢。
- 取链时自动降到可用最高音质（配置音质不存在时）；拉流失败不降级，
  同一链接重试 3 次失败即提示「此曲不可用」。
- 封面 APIC 的 MIME 已按文件头 magic 检测（JPEG/PNG/GIF/BMP/WebP，未知回退 jpeg）。
- 缓存清理时间基准 bug 已修复（见 `问题.local.md` 更新记录）。
- 歌词为 USLT 非同步文本；同步滚动歌词（SYLT）尚未实现，待 mock 验证后可选加入。
