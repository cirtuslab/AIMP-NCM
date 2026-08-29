# AIMP NCM（网易云串流）

![Language](https://img.shields.io/badge/language-C%2B%2B-00599C)
![Version](https://img.shields.io/github/v/release/cirtuslab/AIMP-NCM?label=version&color=blue)
![License](https://img.shields.io/badge/license-MIT-green)
![AIMP](https://img.shields.io/badge/AIMP-5%2B-orange)

适用于 [AIMP](https://www.aimp.ru/) 5.x 的网易云音乐串流插件。

将网易云歌单同步到 AIMP 的「网易云串流」播放列表；播放时，插件内置的本地代理会实时解析真实
CDN 链接并把音频流交给 AIMP，同时在流中注入标题、歌手、专辑、封面与歌词，让播放器界面、
封面面板和歌词功能直接显示歌曲信息。音频会缓存到本地，支持拖动进度。

## v1.5.0 概览

- 歌单一键同步，超过 1000 首的大歌单自动分页拉取。
- 播放时实时取链，链接不过期；音频落盘缓存，支持拖动进度（seek）。
- 歌曲信息流内注入：标题、歌手、专辑、封面直接显示，可选注入歌词（原词 + 翻译合并）。
- 八档音质可选，配置音质不可用时自动降级。
- 粘贴 `MUSIC_U` Cookie 即登录，直连网易云或经本地镜像服务（Node / Python）均可。

## 功能特性

- 网易云歌单同步到 AIMP 播放列表「网易云串流」（同名自动复用，不重复创建）
- 播放时实时解析 CDN 链接并代理音频流，无需下载整张歌单
- 流内注入 ID3v2.3 元数据：标题、歌手、专辑、封面（APIC）
- 可选歌词注入（USLT）：原词 + 翻译合并，mp3 / flac / wav 均验证可用
- 音质选择：`standard` / `higher` / `exhigh` / `lossless` / `hires` /
  `jymaster` / `jyeffect` / `sky`，不可用时自动降级
- 磁盘缓存：重复播放不重复下载，支持 Range / 拖动进度
- 直连（weapi/eapi）或镜像模式；镜像服务可选 Node 或 Python 实现

## 安装

双击 `aimp_ncm.aimppack` 即可完成安装。

也可通过 AIMP 安装：主菜单 → 选项... → 插件 → 安装，选择 `.aimppack`，重启 AIMP 生效。

## 使用

1. 打开 AIMP：主菜单 → 选项... → 插件 →「网易云串流」。
2. 粘贴网易云 `MUSIC_U` Cookie（自动补键名、去引号与空白）。
3. 勾选需要同步的歌单，点击「应用」，AIMP 播放列表即出现「网易云串流」。
4. 在设置页可调整音质、缓存保留天数、歌词注入与直连/镜像模式。

配置保存在 `%APPDATA%\AIMP\NcmPlugin\config.json`。

## 注意事项

- 登录 Cookie 以明文保存在 `config.json`，请妥善保管。
- 本地代理仅绑定 `127.0.0.1`，请勿暴露到公网。
- 本项目仅供个人学习研究，与网易云音乐官方无关；使用非官方接口可能违反服务条款，风险自负。

## 其他

作者：cirtuslab / YuzuBD

构建与打包：运行 `build.ps1`（x86 + x64 构建）与 `release.ps1`（生成 `aimp_ncm.aimppack`）。
