#pragma once
#include <string>
#include <vector>
#include <windows.h>

struct NcmConfig {
    std::wstring cookie;          // MUSIC_U + __csrf etc
    std::wstring apiUrl = L""; // 代理镜像地址，可选；为空则直连 music.163.com
    std::wstring quality = L"exhigh"; // standard/higher/exhigh/lossless/hires/jymaster/jyeffect/sky
    std::vector<long long> selectedPlaylists; // playlist ids
    bool useProxy = false; // true 时走 apiUrl 代理，false 时直连 (默认直连，代理可选)
    bool lazyM3U = false; // true 时写 ncm:// 懒加载秒级，false 时批量预取真实链接
    std::wstring uid; // user id after login
    int localPort = 47777; // 本地重定向服务端口(播放列表 http://127.0.0.1 条目使用)
};

class ConfigManager {
public:
    static std::wstring GetConfigPath();
    static bool Load(NcmConfig& cfg);
    static bool Save(const NcmConfig& cfg);
    static bool Clear();
};
