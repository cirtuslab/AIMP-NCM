#include "config.h"
#include "utils.h"
#include <shlobj.h>
#include <mutex>
#include <fstream>
#include <dpapi.h>
#include <vector>
#include "../third_party/nlohmann/json.hpp"
#pragma comment(lib, "crypt32.lib")
using json = nlohmann::json;

// M1: 设置页(主线程)、播放连接线程、后台任务线程都会 Load/Save 配置:
//     1) 全部读写走同一把互斥锁, 避免并发读到截断的 JSON 或互相覆盖;
//     2) 落盘先写 .tmp 再原子替换(D4 方案, 此前 config.json 未纳入);
//     3) 提供字段级更新入口(UpdateUid/UpdateDeviceCookie), 后台线程只改单字段,
//        避免用旧快照整份回写、把用户刚保存的其它设置(如 Cookie)回滚。
static std::mutex g_cfgMtx;

std::wstring ConfigManager::GetConfigDir() {
    WCHAR path[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path);
    std::wstring dir = std::wstring(path) + L"\\AIMP\\NcmPlugin";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ConfigManager::GetConfigPath() {
    return GetConfigDir() + L"\\config.json";
}

namespace {

// ---- 凭据加密: DPAPI(当前用户) + base64 落盘 ----
// 登录 Cookie / 镜像 Token / 本地代理 Token 属于凭据, 不再明文写入 config.json;
// 加密数据经 base64 编码后以可读文本存储(兼容 JSON/备份/迁移)。

std::string Base64Encode(const std::string& s){
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    for(size_t i = 0; i < s.size(); i += 3){
        unsigned a = (unsigned char)s[i];
        unsigned b = i + 1 < s.size() ? (unsigned char)s[i + 1] : 0;
        unsigned c = i + 2 < s.size() ? (unsigned char)s[i + 2] : 0;
        out += T[a >> 2];
        out += T[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < s.size()) ? T[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < s.size()) ? T[c & 63] : '=';
    }
    return out;
}
bool Base64Decode(const std::string& s, std::string& out){
    static const signed char T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    out.clear();
    out.reserve((s.size() / 4) * 3);
    unsigned buf = 0; int bits = 0;
    for(char ch : s){
        if(ch == '=' || ch == '\r' || ch == '\n') continue;
        signed char v = T[(unsigned char)ch];
        if(v < 0) return false;
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if(bits >= 8){
            bits -= 8;
            out.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    return true;
}

bool ProtectSecret(const std::string& plain, std::string& out){
    if(plain.empty()){ out.clear(); return true; }
    DATA_BLOB in{(DWORD)plain.size(), (BYTE*)plain.data()};
    DATA_BLOB enc{};
    if(!CryptProtectData(&in, L"AIMP NCM 凭据", nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &enc)) return false;
    out.assign((const char*)enc.pbData, enc.cbData);
    LocalFree(enc.pbData);
    return true;
}
bool UnprotectSecret(const std::string& blob, std::string& out){
    if(blob.empty()){ out.clear(); return true; }
    DATA_BLOB in{(DWORD)blob.size(), (BYTE*)blob.data()};
    DATA_BLOB dec{};
    if(!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &dec)) return false;
    out.assign((const char*)dec.pbData, dec.cbData);
    LocalFree(dec.pbData);
    return true;
}

// 读取加密字段: 先 base64 解码再 DPAPI 解密; 失败返回空
std::wstring LoadSecret(const json& j, const char* encField, const char* plainField){
    if(j.contains(encField)){
        std::string b64 = j.value(encField, "");
        if(!b64.empty()){
            std::string blob, plain;
            if(Base64Decode(b64, blob) && UnprotectSecret(blob, plain))
                return Utf8ToWide(plain);
            return L"";   // 解密失败(换用户/换机器): 视为无凭据, 避免读到垃圾
        }
        // encField 为空(DPAPI 加密失败时回退明文存储): 读旧明文字段
        return Utf8ToWide(j.value(plainField, ""));
    }
    // 旧版明文字段(兼容升级)
    return Utf8ToWide(j.value(plainField, ""));
}
void SaveSecret(json& j, const char* encField, const char* plainField, const std::wstring& secret){
    std::string plain = WideToUtf8(secret);
    std::string blob;
    if(plain.empty() || (ProtectSecret(plain, blob) && !blob.empty())){
        j[encField] = blob.empty() ? "" : Base64Encode(blob);
        j[plainField] = "";    // 清理旧明文
    } else {
        // DPAPI 失败(极少见): 回退明文并保留旧字段, 保证配置仍可用
        j[plainField] = plain;
        j[encField] = "";
    }
}

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
    cfg.cookie = LoadSecret(j, "cookieEnc", "cookie");
    cfg.apiUrl = NormalizeApiUrl(Utf8ToWide(j.value("apiUrl","")));
    cfg.quality = Utf8ToWide(j.value("quality","exhigh"));
    // 兼容旧字段 useDirectApi / 新字段 useProxy
    if(j.contains("useProxy")) cfg.useProxy = j.value("useProxy", false);
    else if(j.contains("useDirectApi")) cfg.useProxy = !j.value("useDirectApi", true);
    else cfg.useProxy = !cfg.apiUrl.empty(); // 若有 apiUrl 则默认启用代理
    cfg.uid = Utf8ToWide(j.value("uid",""));
    cfg.mirrorToken = LoadSecret(j, "mirrorTokenEnc", "mirrorToken");
    cfg.localPort = j.value("localPort", 47777);
    cfg.cacheDays = j.value("cacheDays", 7);
    cfg.cacheWhitelist = Utf8ToWide(j.value("cacheWhitelist",""));
    cfg.lyricMode = Utf8ToWide(j.value("lyricMode","uslt"));
    cfg.deviceCookie = Utf8ToWide(j.value("deviceCookie",""));
    cfg.localToken = LoadSecret(j, "localTokenEnc", "localToken");
    cfg.selectedPlaylists.clear();
    if (j.contains("selectedPlaylists") && j["selectedPlaylists"].is_array()) {
        for (auto& v : j["selectedPlaylists"]) cfg.selectedPlaylists.push_back(v.get<long long>());
    }
    return cfg;
}
json DumpConfig(const NcmConfig& cfg){
    json j;
    SaveSecret(j, "cookieEnc", "cookie", cfg.cookie);
    j["apiUrl"] = WideToUtf8(cfg.apiUrl);
    j["quality"] = WideToUtf8(cfg.quality);
    j["useProxy"] = cfg.useProxy;
    j["uid"] = WideToUtf8(cfg.uid);
    SaveSecret(j, "mirrorTokenEnc", "mirrorToken", cfg.mirrorToken);
    j["localPort"] = cfg.localPort;
    j["cacheDays"] = cfg.cacheDays;
    j["cacheWhitelist"] = WideToUtf8(cfg.cacheWhitelist);
    j["lyricMode"] = WideToUtf8(cfg.lyricMode);
    j["deviceCookie"] = WideToUtf8(cfg.deviceCookie);
    SaveSecret(j, "localTokenEnc", "localToken", cfg.localToken);
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
