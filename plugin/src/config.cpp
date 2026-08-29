#include "config.h"
#include "utils.h"
#include <shlobj.h>
#include <mutex>
#include <fstream>
#include "../third_party/nlohmann/json.hpp"
using json = nlohmann::json;

// M1: 设置页(主线程)、播放连接线程、后台任务线程都会 Load/Save 配置:
//     1) 全部读写走同一把互斥锁, 避免并发读到截断的 JSON 或互相覆盖;
//     2) 落盘先写 .tmp 再原子替换(D4 方案, 此前 config.json 未纳入);
//     3) 提供字段级更新入口(UpdateUid/UpdateDeviceCookie), 后台线程只改单字段,
//        避免用旧快照整份回写、把用户刚保存的其它设置(如 Cookie)回滚。
static std::mutex g_cfgMtx;

std::wstring ConfigManager::GetConfigPath() {
    WCHAR path[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path);
    std::wstring dir = std::wstring(path) + L"\\AIMP\\NcmPlugin";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

namespace {

// H3: 用宽字符路径构造 fstream(MSVC 扩展重载), 兼容非 ASCII 用户名/临时目录。
//     旧实现先 WideToUtf8 再用窄字符重载, MSVC 会把窄路径按系统 ANSI 代码页解码,
//     中文用户名的 %APPDATA%/TEMP 路径必然打开失败 → 配置读写整体失效。
bool ReadJsonFile(const std::wstring& path, json& j){
    std::ifstream f(path.c_str());
    if(!f) return false;
    try{ f >> j; return true; }catch(...){ return false; }
}
bool WriteJsonFile(const std::wstring& path, const json& j){
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp.c_str(), std::ios::trunc);
        if(!f) return false;
        f << j.dump(2);
    }
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

NcmConfig ParseConfig(const json& j){
    NcmConfig cfg;
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
    return cfg;
}
json DumpConfig(const NcmConfig& cfg){
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
    return j;
}

// 以下两个函数要求调用方已持有 g_cfgMtx
bool LoadLocked(NcmConfig& cfg){
    json j;
    if(!ReadJsonFile(ConfigManager::GetConfigPath(), j)) return false;
    try{ cfg = ParseConfig(j); }catch(...){ return false; }   // 字段类型异常时保持默认值
    return true;
}
bool SaveLocked(const NcmConfig& cfg){
    return WriteJsonFile(ConfigManager::GetConfigPath(), DumpConfig(cfg));
}

} // namespace

bool ConfigManager::Load(NcmConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    return LoadLocked(cfg);
}
bool ConfigManager::Save(const NcmConfig& cfg) {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    return SaveLocked(cfg);
}
bool ConfigManager::Clear() {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    auto p = GetConfigPath();
    DeleteFileW(p.c_str());
    return true;
}
bool ConfigManager::UpdateUid(const std::wstring& uid) {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    NcmConfig cfg; LoadLocked(cfg);
    cfg.uid = uid;
    return SaveLocked(cfg);
}
bool ConfigManager::UpdateDeviceCookie(const std::wstring& deviceCookie) {
    std::lock_guard<std::mutex> lk(g_cfgMtx);
    NcmConfig cfg; LoadLocked(cfg);
    cfg.deviceCookie = deviceCookie;
    return SaveLocked(cfg);
}
