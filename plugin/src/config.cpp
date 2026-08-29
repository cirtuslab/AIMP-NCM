#include "config.h"
#include "utils.h"
#include <shlobj.h>
#include <fstream>
#include "../third_party/nlohmann/json.hpp"
using json = nlohmann::json;

std::wstring ConfigManager::GetConfigPath() {
    WCHAR path[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path);
    std::wstring dir = std::wstring(path) + L"\\AIMP\\NcmPlugin";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}
bool ConfigManager::Load(NcmConfig& cfg) {
    auto p = GetConfigPath();
    std::ifstream f(WideToUtf8(p));
    if (!f) return false;
    try {
        json j; f >> j;
        cfg.cookie = Utf8ToWide(j.value("cookie",""));
        cfg.apiUrl = NormalizeApiUrl(Utf8ToWide(j.value("apiUrl","")));
        cfg.quality = Utf8ToWide(j.value("quality","exhigh"));
        // 兼容旧字段 useDirectApi / 新字段 useProxy
        if(j.contains("useProxy")) cfg.useProxy = j.value("useProxy", false);
        else if(j.contains("useDirectApi")) cfg.useProxy = !j.value("useDirectApi", true);
        else cfg.useProxy = !cfg.apiUrl.empty(); // 若有 apiUrl 则默认启用代理
        cfg.uid = Utf8ToWide(j.value("uid",""));
        cfg.localPort = j.value("localPort", 47777);
        cfg.cacheDays = j.value("cacheDays", 7);
        cfg.cacheWhitelist = Utf8ToWide(j.value("cacheWhitelist",""));
        cfg.lyricMode = Utf8ToWide(j.value("lyricMode","uslt"));
        cfg.deviceCookie = Utf8ToWide(j.value("deviceCookie",""));
        cfg.selectedPlaylists.clear();
        if (j.contains("selectedPlaylists") && j["selectedPlaylists"].is_array()) {
            for (auto& v : j["selectedPlaylists"]) cfg.selectedPlaylists.push_back(v.get<long long>());
        }
        return true;
    } catch(...) { return false; }
}
bool ConfigManager::Save(const NcmConfig& cfg) {
    auto p = GetConfigPath();
    json j;
    j["cookie"] = WideToUtf8(cfg.cookie);
    j["apiUrl"] = WideToUtf8(cfg.apiUrl);
    j["quality"] = WideToUtf8(cfg.quality);
    j["useProxy"] = cfg.useProxy;
    j["uid"] = WideToUtf8(cfg.uid);
    j["localPort"] = cfg.localPort;
    j["cacheDays"] = cfg.cacheDays;
    j["cacheWhitelist"] = WideToUtf8(cfg.cacheWhitelist);
    j["lyricMode"] = WideToUtf8(cfg.lyricMode);
    j["deviceCookie"] = WideToUtf8(cfg.deviceCookie);
    j["selectedPlaylists"] = cfg.selectedPlaylists;
    std::ofstream f(WideToUtf8(p));
    if (!f) return false;
    f << j.dump(2);
    return true;
}
bool ConfigManager::Clear() {
    auto p = GetConfigPath();
    DeleteFileW(p.c_str());
    return true;
}
