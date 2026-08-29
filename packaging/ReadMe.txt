AIMP NCM (网易云串流)
=====================

[v1.5] Stream NetEase Cloud Music playlists into AIMP 5.x.

Playlist entries are served through the plugin's built-in local proxy
(http://127.0.0.1), which resolves the real CDN link on playback, injects an
ID3v2.3 tag (title / artist / album / duration / cover / optional USLT lyrics)
into the stream, and caches audio for Range/seek.

Features:
- QR-code login / paste MUSIC_U cookie (browser import available in GUI)
- Quality selection: standard / higher / exhigh / lossless / hires /
  jymaster / jyeffect / sky (auto fallback)
- One-click playlist sync to the "网易云串流" playlist
- In-stream metadata + cover + lyrics (USLT, toggleable)
- Direct (weapi/eapi) or optional mirror mode

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

NOTICE
------
- Login cookie (MUSIC_U) is stored in plaintext in config.json; keep it safe.
- The local proxy binds to 127.0.0.1 only; do not expose it publicly.
- This project is for personal study only; it is not affiliated with
  NetEase Cloud Music, and using unofficial APIs may violate the service
  terms. Use at your own risk.

Project: https://github.com/cirtuslab/AIMP-NCM
Issues:  https://github.com/cirtuslab/AIMP-NCM/issues

Author: cirtuslab / YuzuBD
