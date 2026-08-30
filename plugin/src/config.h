#pragma once
#include <string>
#include <vector>
#include <windows.h>

struct NcmConfig {
    std::wstring cookie;          // MUSIC_U + __csrf etc (DPAPI 加密落盘)
    std::wstring apiUrl = L""; // 代理镜像地址，可选；为空则直连 music.163.com
    std::wstring mirrorToken;     // 镜像服务共享 Token(可选, DPAPI 加密落盘; 请求时经 X-NCM-Token 头发送)
    std::wstring quality = L"exhigh"; // standard/higher/exhigh/lossless/hires/jymaster/jyeffect/sky
    std::vector<long long> selectedPlaylists; // playlist ids
    bool useProxy = false; // true 时走 apiUrl 代理，false 时直连 (默认直连，代理可选)
    std::wstring uid; // user id after login
    int localPort = 47777; // 本地重定向服务端口(播放列表 http://127.0.0.1 条目使用)
    int cacheDays = 7;     // 播放缓存保留天数; <=0 表示永不自动删除
    std::wstring cacheWhitelist; // 白名单歌单ID(逗号分隔), 其歌曲缓存永不删除
    std::wstring lyricMode = L"uslt"; // none=不注入歌词 / uslt=流内注入 USLT 歌词帧(已验证 mp3/flac/wav)
    std::wstring deviceCookie;   // 设备指纹 Cookie(首次生成后持久化, 仅登录/换 Cookie 时刷新)
    std::wstring localToken;     // 本地代理访问 token(随机生成, DPAPI 加密落盘; 播放列表 URL 携带)
};

class ConfigManager {
public:
    // 配置目录(%APPDATA%\AIMP\NcmPlugin, 自动创建)。config.json/song_meta.json/
    // ncm_playlist.m3u8 等持久文件统一放这里, 与 %TEMP% 下的播放缓存分开。
    static std::wstring GetConfigDir();
    static std::wstring GetConfigPath();
    static bool Load(NcmConfig& cfg);
    static bool Save(const NcmConfig& cfg);
    static bool Clear();
    // M1: 字段级更新(锁内 Load-Modify-Save)。后台线程只改单字段,
    //     避免用旧快照整份回写、覆盖用户并发保存的其它设置。
    static bool UpdateUid(const std::wstring& uid);
    static bool UpdateDeviceCookie(const std::wstring& deviceCookie);
};
