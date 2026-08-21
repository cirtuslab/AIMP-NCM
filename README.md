# AIMP NCM 网易云串流 - GUI 最终交付版

> **一键 GUI** + **AIMP 插件** 双模式，支持 **扫码登录 / 音质选择 / 歌单同步到播放列表**，基于 `NeteaseCloudMusicApi`。

## 交付物

- `gui/app.py` **GUI 控制中心** (Tkinter, 开箱即用) - 推荐
- `dist/aimp_ncm.aimppack` AIMP 5.40 插件 (x86+x64)
- `ncm_service/server.js` Node 镜像 / `ncm_service/app.py` Python 镜像

## 功能

- **GUI 一键操作**: 二维码扫码登录 (qrcode/Pillow) / 音质下拉 / 歌单勾选 / 一键同步到 AIMP `gui/app.py:1`
- **登录**：二维码扫码（`login/qrcode`）+ Cookie 直填，持久化到 `%APPDATA%\AIMP\NcmPlugin\config.json` `plugin/src/config.cpp:7`
- **官方网页登录（推荐，无风控）**：在浏览器正常登录 music.163.com 后，GUI 点「从浏览器导入」自动读取 Cookie（支持 Firefox/Edge/Chrome/Chromium 多 Profile）；Chrome 新版加密无法读取时按引导粘贴即可
- **风控滑块 challenge**：扫码偶发 8821 验证时会自动弹出滑块页面，完成后自动继续登录 `plugin/src/ncm_client.cpp`
- **音质**：`standard / higher / exhigh / lossless / hires / jymaster / jyeffect / sky`，实时生效
- **歌单**：自动拉取 `user/playlist`，多选同步，生成 `m3u8` 并 `AIMP.exe m3u` 导入播放列表
- **播放列表集成**：支持分组、搜索、拖拽，链接10分钟过期后重跑同步刷新

## 架构

```
GUI (gui/app.py, Tkinter) ──► NCM API 镜像 (http://localhost:3000) ──► 网易云
   ├─ 扫码登录 (qrcode) -> config.json
   ├─ 刷新歌单 -> Treeview 勾选
   └─ 同步 -> %TEMP%/aimp_ncm/ncm_*.m3u8 -> AIMP.exe

AIMP.exe ── aimp_ncm.dll (C++17, WinHTTP, BCrypt) ──┐
             └─ 读取同一 config.json (可选)            │
镜像服务 (二选一):                                   │
  ncm_service/server.js (Node, 推荐) ── NeteaseCloudMusicApiBackup (30k★)
  ncm_service/app.py   (Python)
```

参考：`AdrianEddy/AIMPYouTube` 的 FileSystem 模式 + `cirtuslab/AIMPLyricsSaver` 的 WinHTTP/RapidJSON。

## 快速开始 (GUI 推荐)

### 0. 代理 (可选)

- **国内直连 (默认)**: 无需镜像，`useProxy=false`，插件/GUI 直接 `weapi/eapi` 到 `music.163.com` (需 `pycryptodome`，已修复 `ncm_crypto.cpp:102 RSA` 直连)。
- **海外/被封**: 勾选 `启用代理` 填 `http://localhost:3000` 或 `http://你的VPS:3000`，需先启动镜像:

```bash
cd ncm_service && npm install && npm start  # http://localhost:3000
# 或 Python: pip install -r ncm_service/requirements.txt && python ncm_service/app.py
# Docker: docker run -d -p 3000:3000 binaryify/netease-cloud-music-api
```

### 1. 启动 GUI (无需编译)

```bash
pip install -r gui/requirements.txt
python gui/app.py
# 或双击 gui/run.bat
# 打包 EXE: powershell -ExecutionPolicy Bypass -File gui/build_exe.ps1  -> dist_gui/AIMP_NCM_GUI.exe
```

**GUI 四步:**
1. 顶部 `API 镜像` 测试连接 (默认 `http://localhost:3000`)
2. ① 登录: 点 `获取二维码` -> 网易云APP扫码 -> 自动轮询成功写入配置
3. ② 音质: 下拉选择 `exhigh/lossless` 等
4. ③ 歌单: 点 `刷新歌单` -> 双击/空格勾选 -> ④ `同步选中歌单到 AIMP` -> 自动生成 `m3u8` 并调用 `AIMP.exe` 打开

### 2. 安装 AIMP 插件 (可选，与 GUI 共用配置)

```powershell
# 自动安装 (已修复 aimppack 结构)
powershell -ExecutionPolicy Bypass -File dist/install.ps1
# 或手动
.\build.ps1  # 需 VS2022/2026，生成 dist/aimp_ncm.aimppack (x86+x64)
# 手动复制: dist/aimp_ncm.dll -> AIMP\Plugins\aimp_ncm\x64\ ; dist/x86\aimp_ncm.dll -> AIMP\Plugins\aimp_ncm\
```

> **关于 UAC 弹窗**：AIMP 装在 `C:\Program Files` 下时，插件目录对普通用户只读，
> 每次双击 aimppack 安装/更新插件 AIMP 都会请求管理员权限（弹 UAC）——这是 AIMP 的
> 正常行为，不是插件问题。若想免 UAC 静默安装，以管理员身份运行一次：
> ```powershell
> powershell -ExecutionPolicy Bypass -File tools\fix_aimp_plugin_perm.ps1
> ```
> 该脚本给 `AIMP\Plugins` 目录授予当前用户写权限（参考
> [aimp_desktop_lyrics](https://github.com/cirtuslab/aimp_desktop_lyrics) 的做法），
> 之后所有插件的 aimppack 双击安装都不再弹 UAC。
插件与 GUI 共用 `%APPDATA%\AIMP\NcmPlugin\config.json`，GUI 同步的歌单也可通过插件的虚拟文件 `ncm://` 播放。

**CLI 备用:**
```bash
python ncm_service/qr_login.py          # 扫码登录
python ncm_service/sync_to_aimp.py --all --quality lossless
```

## 配置文件

`%APPDATA%\AIMP\NcmPlugin\config.json`
```json
{
  "cookie": "MUSIC_U=...; __csrf=...",
  "apiUrl": "http://localhost:3000",
  "quality": "exhigh",
  "uid": "123456",
  "selectedPlaylists": [123456789, 987654321]
}
```

## 关键接口

| 功能 | 镜像路径 | 原始 weapi |
|------|----------|------------|
| 二维码 key | `GET /login/qr/key` | `POST /api/login/qrcode/unikey` |
| 二维码检查 | `GET /login/qr/check?key=xxx` | `POST /api/login/qrcode/client/login` |
| 用户歌单 | `GET /user/playlist?uid=xxx` | `weapi /api/user/playlist` |
| 歌单全部歌曲 | `GET /playlist/track/all?id=xxx` | `weapi /api/v6/playlist/detail` + `api/v3/song/detail` |
| 播放 URL | `GET /song/url/v1?id=xxx&level=exhigh` | `eapi /api/song/enhance/player/url/v1` |

## 常见问题

- **播灰色/404？** 会员音质或版权限制，插件自动降级到 `exhigh`，可接入 `UnblockNeteaseMusic` 作为音源替换。
- **境外 460/403？** 配置 `AIMP_NCM_API=http://国内VPS:3000`。
- **URL 过期？** 插件每次 `CreateStream` 实时请求新 URL，无需担心。
- **AIMP 32 位？** 本工程默认 x64，32 位需 `-A Win32` 重新编译。

## 许可

MIT。仅供学习，遵守网易云 ToS。第三方：AIMP SDK © Artem Izmaylov, nlohmann/json MIT, NeteaseCloudMusicApi MIT。
