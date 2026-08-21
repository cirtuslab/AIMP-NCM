# AIMP NCM GUI 控制中心

图形化最终交付项目，一键完成 网易云登录 / 音质选择 / 歌单同步到 AIMP。

## 运行

```bash
pip install -r gui/requirements.txt
python gui/app.py
# 或双击 gui/run.bat
```

## 打包为 EXE (可选)

```bash
pip install pyinstaller
pyinstaller --onefile --windowed --name AIMP_NCM_GUI --icon=gui/icon.ico gui/app.py
# 生成 dist/AIMP_NCM_GUI.exe
```

## 界面说明

- **API 镜像**: 默认 `http://localhost:3000`，点`测试连接`，可点`启动本地镜像`自动 `node ncm_service/server.js`
- **① 登录**: `获取二维码` -> 用网易云APP扫码 -> 自动轮询 `qr_login.py` 逻辑，成功写入 `%APPDATA%\AIMP\NcmPlugin\config.json`，也支持粘贴 `MUSIC_U` Cookie。
- **② 音质**: 下拉选择 `standard/higher/exhigh/lossless/hires/jymaster/jyeffect/sky`，自动保存。
- **③ 歌单**: `刷新歌单` 拉取 `user/playlist`，双击或空格勾选，支持全选。
- **④ 同步**: `同步选中歌单到 AIMP` -> 对每个歌单调用 `playlist/track/all` + `song/url/v1` 生成 `%TEMP%\aimp_ncm\ncm_*.m3u8` 并自动 `AIMP.exe m3u` 打开，新播放列表即出现。也可 `生成 m3u8 到桌面` 手动导入。

日志区显示实时状态，配置与插件共用 `config.json`。

## 依赖

- `requests`, `qrcode`, `Pillow` (见 `gui/requirements.txt`)
- 镜像服务 `ncm_service/server.js` (Node) 或直连 (需 pycryptodome)

## 与插件关系

- 无 GUI 插件 `aimp_ncm.dll` 通过读取同一 `config.json` 提供 `ncm://` 虚拟文件支持，GUI 是其配置前端。
- 也可完全不用插件，仅用 GUI 生成的 `m3u8` 直接用 AIMP 播放。

## 常见问题

- 二维码不显示: `pip install qrcode Pillow` 或查看 `https://api.qrserver.com/...` 链接。
- 刷新歌单 0 个: 未登录或 UID 未获取，重新扫码。
- 同步 0 首: 音质无权限或链接过期，重试或降低音质到 `exhigh`。
