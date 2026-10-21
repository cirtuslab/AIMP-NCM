# AIMP NCM · NetEase Cloud Music streaming plugin

> **AIMPack plugin** (AIMP 5.x, x86 + x64): play your NetEase Cloud Music playlists directly inside AIMP.
> Cookie login, eight quality tiers, one-click playlist sync. A loopback proxy hands the real audio
> stream to the player and injects full track metadata (title / artist / album / cover / optional
> lyrics) into the stream itself; audio is cached to disk and seeking works.

**English** | [中文](README.md)

![Language](https://img.shields.io/badge/language-C%2B%2B-00599C)
![Version](https://img.shields.io/github/v/release/cirtuslab/AIMP-NCM?label=version&color=blue)
![License](https://img.shields.io/github/license/cirtuslab/AIMP-NCM)
![AIMP](https://img.shields.io/badge/AIMP-5%2B-orange)
![Build](https://github.com/cirtuslab/AIMP-NCM/actions/workflows/windows-build.yml/badge.svg)

## Features

- **Native options page** — lives under `Settings → Plugins → NetEase Stream` and follows the AIMP skin.
- **Cookie login** — paste `MUSIC_U` and you are in (the key name, quotes and whitespace are normalised
  automatically; a bare value works too).
- **Playlist sync** — tick playlists → click *Apply* → a `NetEase Stream` playlist is created or reused
  (never duplicated), with the selection persisted. Playlists over 1000 tracks are fetched with paging.
- **Metadata injected into the stream** — an ID3v2.3 tag (title / artist / album / APIC cover) is
  prepended to the audio stream at playback time, so AIMP's player and cover panel show real data.
  Covers are downloaded on first play and cached on disk — **no batch requests**.
- **Optional lyrics injection** — a USLT frame, with original and translated lines merged by nearest
  timestamp (±350 ms). Verified on mp3 / flac / wav; toggleable in the options page.
- **Loopback proxy playback** — an embedded `http://127.0.0.1:{port}` server (default 47777, auto-increments
  when taken) resolves the real CDN URL at play time and proxies the stream, with disk caching and
  Range/seek support — stream URLs never go stale, nothing has to be prefetched. Failed downloads retry
  (up to 3 attempts on the same URL) before a "track unavailable" notice.
- **Failure notice can be turned off** — delisted / unlicensed tracks show a dialog by default
  (**silent, no system beep**), or can be set to *skip automatically* — diagnostic log only, playback is
  not interrupted.
- **Eight quality tiers** — `standard / higher / exhigh / lossless / hires / jymaster / jyeffect / sky`.
  If the configured tier is unavailable the plugin falls back to the highest tier the account can use
  (higher tiers need a membership); a change takes effect on the **next** track.
- **Disk cache policy** — retention of 1 / 3 / 7 / 30 days or never, plus a whitelist of playlists whose
  cache is never pruned.
- Direct connection works from inside mainland China; outside or on restricted networks a proxy mirror
  (Node or Python) can be enabled.
- Restricted responses (401/402/403/429/451) raise a single dialog with a 60 s cooldown so playback is
  not flooded with popups.

## How it works

AIMP 5 exposes three extension points this plugin combines:

1. a **file-system extension** that registers synthetic playlist entries (`http://127.0.0.1:{port}/x/{token}/{playlistId}/{songId}.mp3`),
2. a **local HTTP server** that, on request, resolves the current CDN URL for that track, opens the
   upstream stream and relays it while honouring `Range` for seeking,
3. an **ID3v2.3 header** written in front of the relayed bytes, so the player sees a normal tagged file
   instead of an anonymous byte stream, plus a **lyrics provider** for the AIMP lyrics panel.

The local server requires a per-install token in the path (compared in constant time) because playlist
entries carry the URL; without it, exporting a playlist would hand out a working stream endpoint.
Credentials are sealed with Windows DPAPI for the current user, so copying a config directory to another
machine or account does not transfer a login.

## Install

Download `aimp_ncm.aimppack` from [Releases](https://github.com/cirtuslab/AIMP-NCM/releases/latest):

- Double-click `aimp_ncm.aimppack` (Windows may ask for UAC elevation), or
- install from inside AIMP: main menu → Options → Plugins → Install, pick the `.aimppack`, restart AIMP.

> Note: an older release shipped `tools/fix_aimp_plugin_perm.ps1`, which granted `Users` write access to
> the `AIMP\Plugins` directory to avoid UAC. That weakens the ACL of a system directory and lets any
> process drop a DLL into the plugin path — it has been removed. Use the UAC install or copy the DLL
> manually.

Build from source (Visual Studio 2022/2026 with the C++ toolchain, plus CMake):

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1     # builds x86 + x64, produces dist/aimp_ncm.aimppack
powershell -ExecutionPolicy Bypass -File release.ps1   # additionally assembles the release archive
```

CI builds both architectures on every push and pull request via
[`.github/workflows/windows-build.yml`](.github/workflows/windows-build.yml).

## Usage (four steps)

1. **Log in** — sign in to music.163.com in a browser, then paste the cookie into the options page
   (F12 → Application/Storage → Cookies → copy the whole `MUSIC_U` value; several hundred characters is
   normal).
2. **Network** — inside mainland China keep the direct connection; outside, tick *Enable proxy* and enter
   a mirror address (e.g. `iwenwiki.com:3000`, with or without `http://`) and press *Test*.
3. **Fetch playlists** — press *Refresh playlists* (the UID is filled in automatically if missing).
4. **Sync** — tick playlists → *Apply* → a `NetEase Stream` playlist appears on the left; double-click to
   play.

## Configuration

The configuration directory is `%APPDATA%\AIMP\NcmPlugin\`; besides `config.json` it also holds the
generated `song_meta.json` (playlist metadata) and `ncm_playlist.m3u8` (playlist hand-off file).

`config.json` fields:

| Field | Meaning |
|---|---|
| `cookieEnc` | Login state (`MUSIC_U=...` etc.), stored **DPAPI-encrypted** (base64); a legacy plaintext `cookie` field is migrated automatically |
| `apiUrl` | Mirror address; empty means direct connection to `music.163.com` |
| `mirrorTokenEnc` | Optional shared mirror token (DPAPI-encrypted; sent as the `X-NCM-Token` header) |
| `useProxy` | `true` uses the mirror, `false` connects directly |
| `quality` | Quality tier, default `exhigh` |
| `uid` | Account ID, filled in when playlists are fetched |
| `localPort` | Local proxy port, default 47777; incremented and written back when taken |
| `cacheDays` | Cache retention in days; `<=0` never deletes (whitelist excepted) |
| `cacheWhitelist` | Playlist IDs (comma separated) whose cache is never pruned |
| `lyricMode` | Lyrics injection: `none` or `uslt` (default) |
| `failNotify` | Failure notice: `dialog` (default, no sound) or `silent` (log only) |
| `deviceCookieEnc` | Device-fingerprint cookie (DPAPI-encrypted; legacy plaintext `deviceCookie` is migrated) |
| `localTokenEnc` | Local proxy access token (random, DPAPI-encrypted; carried in playlist URLs) |
| `selectedPlaylists` | Array of playlist IDs selected for sync |

> Credential fields (`cookieEnc` / `mirrorTokenEnc` / `localTokenEnc` / `deviceCookieEnc`) use Windows
> DPAPI and can only be decrypted by the same Windows user; do not copy `config.json` to another machine
> or account. A legacy plaintext config is encrypted automatically on the first save.
> TLS relaxation (for self-signed mirrors) applies only to the configured mirror host; direct NetEase and
> CDN connections are always verified strictly.

## Cache and logs

Playback scratch data lives under `%TEMP%\aimp_ncm\` (the whole directory can be deleted at any time;
metadata and configuration in `%APPDATA%\AIMP\NcmPlugin\` are unaffected):

| Path | Contents |
|---|---|
| `cache\{pid}_{tid}.{ext}` | Audio stream cache (incomplete downloads are `.part` files and never count as cache hits) |
| `artwork\{tid}.img` | Cover cache (downloaded on first play of that track) |
| `lyric\{tid}.lrc` | Lyrics cache |
| `logs\aimp_ncm_fs.log` | Diagnostic log (3 MB cap, rolling, 7 days) |

## Server-side mirror (optional)

A relay for networks outside mainland China. Two implementations, pick one (default port 3000, both bind
**127.0.0.1 only**):

```bash
cd ncm_service && npm install && node server.js
# or Python:
pip install -r ncm_service/requirements.txt && python ncm_service/app.py
```

The mirror accepts a `cookie` parameter by design, so it should only run on the local machine or a
trusted LAN — never expose it to the internet.

For public or multi-host use, enable shared-token authentication (the *Mirror token* field in the plugin
options):

```bash
NCM_MIRROR_TOKEN=your-random-secret node server.js        # Node
# or
NCM_MIRROR_TOKEN=your-random-secret python app.py          # Python
```

With a token set, every request must carry the `X-NCM-Token` header (the plugin sends it automatically)
or it returns 403. The mirror also only exposes the endpoints the plugin uses; everything else 404s.
Put a TLS reverse proxy in front of it if it is reachable from outside.

## FAQ

- **No track info or cover in the player?** Press *Apply* once more to re-sync playlists (metadata is
  written to `song_meta.json` during sync); covers are fetched on first play and skipped silently when
  unavailable, without affecting playback.
- **Greyed out / 404?** Membership-only quality tier or a licensing restriction. URL resolution falls
  back to the best available quality; stream failures do not downgrade, they retry 3 times and then show
  a "track unavailable" notice.
- **460 / 403 abroad?** Enable the proxy and point it at a mirror hosted inside mainland China.
- **The *Apply* button is greyed out?** Press *Refresh playlists* first; it enables once playlists are
  ticked.
- **Old playlists return 403 after the v1.5.3 security hardening?** The local proxy now requires a token;
  press *Apply* once to re-sync the playlists.
- **401 / 403 / 429?** The cookie may have expired or triggered rate limiting: paste `MUSIC_U` again, or
  retry later / switch mirrors.
- **Does it work with a Chinese Windows username?** Yes. Since v1.5.1 configuration and metadata I/O use
  wide-character paths and no longer depend on the ANSI code page.
- **Quality change had no effect?** It applies from the next track; the current stream keeps its quality.

## Projects this one learned from

The protocol implementation and the AIMP integration borrowed heavily from these open-source projects:

| Project | What was borrowed |
|------|-----------|
| [Binaryify/NeteaseCloudMusicApi](https://github.com/Binaryify/NeteaseCloudMusicApi) | Basis for the Node mirror; reference behaviour for the weapi/eapi endpoints |
| [NeteaseCloudMusicApiEnhanced/api-enhanced](https://github.com/NeteaseCloudMusicApiEnhanced/api-enhanced) | Device-fingerprint and cookie object handling |
| [go-musicfox/go-musicfox](https://github.com/go-musicfox/go-musicfox) | Device-fingerprint cookies (`sDeviceId`, `_ntes_nuid`, …) and the eapi song-URL parameters |
| [chaunsin/netease-cloud-music](https://github.com/chaunsin/netease-cloud-music) | Protocol research notes (eapi encryption details) |
| [AdrianEddy/AIMPYouTube](https://github.com/AdrianEddy/AIMPYouTube) | How to register a file-system extension and play a virtual stream |
| [cirtuslab/aimp_desktop_lyrics](https://github.com/cirtuslab/aimp_desktop_lyrics) | Layout patterns for native options-page controls; the UAC-free plugin-directory approach (since removed here) |
| [cirtuslab/AIMPLyricsSaver](https://github.com/cirtuslab/AIMPLyricsSaver) | C++ plugin pattern with WinHTTP + RapidJSON; lyric translation merging |
| [cirtuslab/SongMetaFixer](https://github.com/cirtuslab/SongMetaFixer) | Track metadata field set and NetEase detail endpoint as a data source |
| [imsyy/SPlayer](https://github.com/imsyy/SPlayer) | The "log in on the official web page, then read the cookie" flow that avoids bot checks |
| [martin211/aimp_dotnet](https://github.com/martin211/aimp_dotnet) | Authoritative mapping of the IIDs used to register each AIMP extension category |

## Licence

Released under the **MIT** licence; the full text is in [LICENSE](LICENSE)
(`Copyright (c) 2026 cirtuslab / YuzuBD`).

Bundled or compiled third-party components and their terms are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): AIMP SDK © Artem Izmaylov (headers under
`plugin/third_party/aimp_sdk/`), nlohmann/json (MIT, compiled into the plugin DLL), NeteaseCloudMusicApi /
express and friends (runtime npm dependencies of the mirror service, not distributed here).
This repository does not contain AIMP itself.

---

## ⚠️ Risk notice

1. **This project is for personal study and research only** — do not use it commercially.
2. Accessing NetEase Cloud Music through unofficial endpoints **may violate the NetEase Cloud Music terms
   of service**; account flags, rate limiting or bans are the user's own responsibility.
3. Login credentials (`MUSIC_U` cookie, mirror token, local proxy token) are stored in `config.json`
   **encrypted with Windows DPAPI (current user)** (base64 fields such as `cookieEnc`). DPAPI is only
   decryptable on that machine by that user; do not copy the configuration directory elsewhere. Before
   uninstalling or handing the device over, clear the cookie in the options page or delete
   `%APPDATA%\AIMP\NcmPlugin\`. A leaked cookie is a leaked account.
4. The plugin opens an HTTP port on the loopback address (127.0.0.1) for playback proxying, reachable
   only from this machine; do not expose it publicly. Playlist entries and
   `%APPDATA%\AIMP\NcmPlugin\ncm_playlist.m3u8` carry the local proxy token — **do not export or share a
   playlist or that m3u8 file**.
5. NetEase Cloud Music endpoints may change at any time and break some or all functionality; the author
   does not promise continued maintenance.
6. This project is **not affiliated with NetEase** in any way and does not represent its position;
   copyrights remain with their respective owners.
7. **Downloading, installing or using this software means you have read and understood the risks above
   and accept full responsibility.** If you disagree, stop using it and uninstall it.
