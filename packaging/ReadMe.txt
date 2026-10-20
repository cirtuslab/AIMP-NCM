AIMP NCM (网易云串流)
=====================

[v1.5.4] Stream NetEase Cloud Music playlists into AIMP 5.x.
安全：凭据 DPAPI 加密、本地代理强制 Token、TLS 仅对配置镜像放宽。

Playlist entries are served through the plugin's built-in local proxy
(http://127.0.0.1), which resolves the real CDN link on playback, injects an
ID3v2.3 tag (title / artist / album / duration / cover / optional USLT lyrics)
into the stream, and caches audio for Range/seek.

Features:
- Paste MUSIC_U cookie to sign in
- Quality selection: standard / higher / exhigh / lossless / hires /
  jymaster / jyeffect / sky (auto fallback)
- One-click playlist sync to the "网易云串流" playlist
- In-stream metadata + cover + lyrics (USLT, toggleable)
- Direct (weapi/eapi) or optional mirror mode
- Failure notifications: dialog (no beep) or silent auto-skip

Install
-------
1. Open AIMP: Main Menu -> Options... -> Plugins -> Install...
2. Select aimp_ncm.aimppack and confirm.
3. Restart AIMP (plugins load at startup).

Alternatively, extract the archive and copy the aimp_ncm folder into the
AIMP Plugins directory.

Configuration
-------------
%APPDATA%\AIMP\NcmPlugin\config.json

Open AIMP Options -> Plugins -> "网易云串流" for the settings page.
Lyrics can be disabled there (歌词注入 -> 不注入) if needed.
Unavailable-track popups can be switched to silent skip there
(失败提示 -> 自动跳过（不提醒）).

Upgrade note (v1.5.4)
---------------------
- Unplayable/de-listed tracks no longer play a system beep.
- New setting 失败提示 lets you silence per-track failure popups entirely
  (the track is then skipped and a line is written to the diagnostic log).
- Lyrics: original text is now requested with lv=-1 (falls back to lv=1),
  fixing tracks whose original lyrics were only available in older
  revisions; metadata lines (作词/作曲/制作人) no longer receive a
  mistimed translation.
- No breaking changes; existing config.json and playlists keep working.

NOTICE
------
- Login cookie (MUSIC_U) and tokens are stored with Windows DPAPI (current
  user) in config.json; do not copy the config folder to another machine/user.
- Playlist URLs and ncm_playlist.m3u8 contain a local proxy token; do not
  export/share the playlist or that file.
- The local proxy binds to 127.0.0.1 only; do not expose it publicly.
- This project is for personal study only; it is not affiliated with
  NetEase Cloud Music, and using unofficial APIs may violate the service
  terms. Use at your own risk.

License
-------
MIT. See the LICENSE and THIRD_PARTY_NOTICES.md files bundled in this package
(AIMP SDK (c) Artem Izmaylov, nlohmann/json MIT, etc.).

Project: https://github.com/cirtuslab/AIMP-NCM
Issues:  https://github.com/cirtuslab/AIMP-NCM/issues

Author: cirtuslab / YuzuBD
