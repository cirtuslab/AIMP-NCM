#include "ncm_client.h"
#include "http_client.h"
#include "ncm_crypto.h"
#include "utils.h"
#include "../third_party/nlohmann/json.hpp"
#include <ctime>
#include <cstdlib>
#include <sstream>
#include <random>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
using json = nlohmann::json;

namespace {

// 解析 LRC 时间戳(支持 mm:ss.xx / mm:ss.xxx), 返回毫秒; 失败返回 false
bool ParseLrcMs(const std::string& ts, int& outMs){
    int mm=0, ss=0, frac=0;
    int n = sscanf_s(ts.c_str(), "%d:%d.%d", &mm, &ss, &frac);
    if(n < 2) return false;
    if(n == 2) frac = 0;
    // 小数位数归一化: 1位->*100, 2位->*10, 3位->*1
    size_t dot = ts.find('.');
    if(dot != std::string::npos){
        int digits = (int)(ts.size() - dot - 1);
        if(digits == 1) frac *= 100;
        else if(digits == 2) frac *= 10;
    }
    outMs = mm*60000 + ss*1000 + frac;
    return true;
}

struct LrcStamp { int ms = 0; std::string raw; };

// 提取一行内全部时间戳与正文起点
bool ExtractLrcStamps(const std::string& line, std::vector<LrcStamp>& stamps, size_t& textPos){
    size_t pos = 0;
    while(pos < line.size() && line[pos] == '['){
        size_t close = line.find(']', pos);
        if(close == std::string::npos) break;
        std::string inner = line.substr(pos+1, close-pos-1);
        int ms = 0;
        if(!ParseLrcMs(inner, ms)) break;
        stamps.push_back({ms, line.substr(pos, close-pos+1)});
        pos = close + 1;
    }
    textPos = pos;
    return !stamps.empty();
}

std::string TrimLrcText(const std::string& text){
    size_t b = text.find_first_not_of(" \t");
    if(b == std::string::npos) return "";
    size_t e = text.find_last_not_of(" \t");
    return text.substr(b, e-b+1);
}

// 合并原词与翻译: 翻译行按时间戳就近匹配(±350ms 容差, 参考 AIMPLyricsSaver),
// 输出 "原词行\n[ts]翻译行"; 无翻译时原样返回原词; 逐字歌词(klyric/yrc)不参与
std::string MergeLyricWithTranslation(const std::string& orig, const std::string& trans){
    if(orig.empty()) return trans;
    if(trans.empty()) return orig;

    struct TransLine { int ms = 0; std::string text; };
    std::vector<TransLine> translated;
    {
        std::istringstream in(trans);
        std::string line;
        while(std::getline(in, line)){
            if(!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<LrcStamp> stamps;
            size_t textPos = 0;
            if(!ExtractLrcStamps(line, stamps, textPos)) continue;
            std::string text = TrimLrcText(line.substr(textPos));
            if(text.empty()) continue;
            for(auto& s : stamps) translated.push_back({s.ms, text});
        }
    }
    if(translated.empty()) return orig;
    std::sort(translated.begin(), translated.end(),
              [](const TransLine& a, const TransLine& b){ return a.ms < b.ms; });

    std::string out;
    std::istringstream in(orig);
    std::string line;
    while(std::getline(in, line)){
        if(!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<LrcStamp> stamps;
        size_t textPos = 0;
        bool hasStamp = ExtractLrcStamps(line, stamps, textPos);
        bool emptyText = hasStamp && TrimLrcText(line.substr(textPos)).empty();
        out += line; out += "\n";
        if(!hasStamp || emptyText) continue;      // 元数据行/空行不挂翻译
        const TransLine* best = nullptr;
        int bestDiff = 351;
        for(auto& c : translated){
            int d = std::abs(c.ms - stamps[0].ms);
            if(d < bestDiff){ bestDiff = d; best = &c; }
        }
        if(best && bestDiff <= 350){
            out += stamps[0].raw + best->text + "\n";
        }
    }
    return out;
}

// 从歌单/详情接口的歌曲 JSON 解析 NcmSong
bool ParseSongJson(const json& s, NcmSong& out){
    out = NcmSong();
    out.id = s.value("id", 0LL);
    out.title = Utf8ToWide(s.value("name",""));
    out.durationMs = s.value("dt", 0);
    if(s.contains("ar") && s["ar"].is_array() && !s["ar"].empty()){
        std::wstring arts;
        for(size_t i=0;i<s["ar"].size();++i){
            if(i) arts += L"/";
            arts += Utf8ToWide(s["ar"][i].value("name",""));
        }
        out.artist = arts;
    }
    if(s.contains("al") && s["al"].is_object()){
        out.album = Utf8ToWide(s["al"].value("name",""));
        if(s["al"].contains("picUrl")) out.coverUrl = Utf8ToWide(s["al"].value("picUrl",""));
    }
    return out.id > 0;
}

} // namespace

// ---- 设备指纹工具（参考 go-musicfox/netease-music） ----

// 生成 52 位随机 hex 的 sDeviceId (用 mt19937+random_device, 避免 rand() 可预测)
static std::string GenSDeviceId(){
    return NcmCrypto::BytesToHex(NcmCrypto::RandomString(26), true);
}
// 从 Cookie 串中提取指定键值
static std::string CookieGet(const std::wstring& cookie, const char* key){
    std::string c = WideToUtf8(cookie);
    std::string k = std::string(key) + "=";
    size_t p = c.find(k);
    if(p==std::string::npos) return "";
    p += k.size();
    size_t e = c.find(';', p);
    std::string v = c.substr(p, e==std::string::npos?std::string::npos:e-p);
    // 去空白
    while(!v.empty() && (v.back()==' '||v.back()=='\t')) v.pop_back();
    while(!v.empty() && (v.front()==' '||v.front()=='\t')) v.erase(v.begin());
    return v;
}
// _ntes_nuid 设备指纹（MD5，参考 go-musicfox GenerateNtesUID）
static std::string GenNtesNuid(){
    // 简化版: 用随机串的 MD5（与官方指纹算法近似即可通过风控）
    std::string raw = std::to_string(time(nullptr)*1000) + NcmCrypto::RandomString(16);
    // MD5
    HCRYPTPROV prov=0;
    HCRYPTHASH hash=0;
    BYTE digest[16]={0};
    DWORD dlen=16;
    if(CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)){
        if(CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)){
            CryptHashData(hash, (BYTE*)raw.data(), (DWORD)raw.size(), 0);
            CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0);
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    char hexbuf[33];
    for(int i=0;i<16;i++) sprintf_s(hexbuf+i*2, 3, "%02x", digest[i]);
    return std::string(hexbuf, 32);
}

NcmClient::NcmClient(const NcmConfig& cfg): cfg_(cfg) {}

std::string NcmClient::RequestMirror(const std::wstring& path, const std::string& jsonData, bool post, int* outStatus){
    std::wstring base = cfg_.apiUrl;
    if(!base.empty() && base.back()==L'/') base.pop_back();
    std::wstring url = base + path;
    json j = json::parse(jsonData.empty()? "{}": jsonData);
    std::string body;
    bool first=true;
    for(auto& kv : j.items()){
        if(!first) body+="&";
        first=false;
        std::string val;
        if(kv.value().is_string()) val=kv.value().get<std::string>();
        else if(kv.value().is_number()) val=kv.value().dump();
        else val=kv.value().dump();
        // url encode val
        std::wstring wval = Utf8ToWide(val);
        std::string enc = HttpClient::UrlEncodeW(wval);
        body += kv.key() + "=" + enc;
    }
    if(!cfg_.cookie.empty()){
        if(!body.empty()) body+="&";
        std::string cenc = HttpClient::UrlEncodeW(cfg_.cookie);
        body += "cookie=" + cenc;
    }
    // 共享 Token 经请求头发送, 避免出现在 URL/日志中;
    // 防御头注入: 仅允许不含 CR/LF 的 token(配置被手动编辑时兜底)
    std::wstring headers;
    if(!cfg_.mirrorToken.empty() &&
       cfg_.mirrorToken.find(L'\r') == std::wstring::npos &&
       cfg_.mirrorToken.find(L'\n') == std::wstring::npos)
        headers = L"X-NCM-Token: " + cfg_.mirrorToken;
    auto resp = HttpClient::Post(url, body, headers, L"");
    if(outStatus) *outStatus = resp.status;
    return resp.body;
}
std::string NcmClient::RequestDirect(const std::wstring& uriPath, const std::string& jsonData, bool useEapi){
    std::wstring url;
    std::string body;
    if(useEapi){
        // eapi 必须发到 interface.music.163.com 且带 header 参数（参考 go-musicfox/netease-music）
        auto e = NcmCrypto::Eapi(WideToUtf8(uriPath), jsonData);
        std::wstring wparams = Utf8ToWide(e.params);
        std::string enc = HttpClient::UrlEncodeW(wparams);
        body = "params=" + enc;
        std::wstring euri = uriPath;
        if(euri.rfind(L"/api",0)==0) euri = L"/eapi" + euri.substr(4);
        url = L"https://interface.music.163.com" + euri;
    } else {
        auto w = NcmCrypto::Weapi(jsonData);
        if(w.encSecKey.empty()) return "";   // C3: RSA 加密失败, 显式失败而不是静默
        std::wstring wparams = Utf8ToWide(w.params);
        std::string encParams = HttpClient::UrlEncodeW(wparams);
        body = "params=" + encParams + "&encSecKey=" + w.encSecKey;
        std::wstring wuri2 = uriPath;
        if(wuri2.rfind(L"/api",0)==0) wuri2 = L"/weapi" + wuri2.substr(4);
        url = L"https://music.163.com" + wuri2;
    }
    // 设备伪装: 完整浏览器 UA + 设备 Cookie（参考 go-musicfox/netease-music）
    // 网易云风控会拦截非浏览器客户端（返回 8821 需要行为验证 / 空 body）
    std::wstring headers =
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36\r\n"
        L"Referer: https://music.163.com\r\n"
        L"Origin: https://music.163.com";
    EnsureDeviceCookie();
    std::wstring cookie = deviceId_;
    if(!cfg_.cookie.empty()){
        if(!cookie.empty()) cookie += L"; ";
        cookie += cfg_.cookie;
    }
    auto r = HttpClient::Post(url, body, headers, cookie);
    if(useEapi){
        try { return NcmCrypto::EapiDecrypt(r.body); } catch(...){ return r.body; }
    }
    return r.body;
}

void NcmClient::EnsureDeviceCookie(){
    if(!deviceId_.empty()) return;
    // 设备指纹持久化在 config.deviceCookie, 只在登录/换 Cookie 时重新生成,
    // 避免每个 NcmClient 实例(每次请求)都换新指纹导致风控
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(!cfg.deviceCookie.empty()){ deviceId_ = cfg.deviceCookie; return; }
    deviceId_ = BuildDeviceCookie();
    // M1: 字段级更新, 避免后台线程用旧快照整份回写、覆盖用户并发保存的其它设置
    ConfigManager::UpdateDeviceCookie(deviceId_);
}
std::wstring NcmClient::BuildDeviceCookie(){
    // 初始化/补全设备 Cookie
    // 参考 NeteaseCloudMusicApiEnhanced processCookieObject + go-musicfox
    // M5: 不再用 srand/rand(会污染宿主进程 AIMP 的全局随机状态),
    //     随机源统一走 NcmCrypto::RandomString(mt19937+random_device)
    std::wstring deviceId_;
    if(deviceId_.empty()){
        deviceId_ = L"os=pc; appver=2.7.1.198277; osver=10; __remember_me=true; "
                    L"deviceId=" + Utf8ToWide(NcmCrypto::RandomString(32)) + L"; "
                    L"ntes_kaola_ad=1; WEVNSM=1.0.0; channel=netease";
    }
    if(CookieGet(deviceId_, "sDeviceId").empty()){
        deviceId_ += L"; sDeviceId=" + Utf8ToWide(GenSDeviceId());
    }
    if(CookieGet(deviceId_, "_ntes_nuid").empty()){
        std::string nuid = GenNtesNuid();
        deviceId_ += L"; _ntes_nuid=" + Utf8ToWide(nuid);
        // _ntes_nnid = <nuid>,<timestamp>
        deviceId_ += L"; _ntes_nnid=" + Utf8ToWide(nuid + "," + std::to_string(time(nullptr)*1000));
    }
    if(CookieGet(deviceId_, "NMTID").empty()){
        deviceId_ += L"; NMTID=" + Utf8ToWide(GenNtesNuid());
    }
    if(CookieGet(deviceId_, "WNMCID").empty()){
        // WNMCID 格式: <6位随机字母>.<时间戳>.01.0
        std::string rnd = NcmCrypto::RandomString(6);
        deviceId_ += L"; WNMCID=" + Utf8ToWide(rnd + "." + std::to_string(time(nullptr)*1000) + ".01.0");
    }
    return deviceId_;
}
void NcmClient::RegenerateDeviceCookie(){
    // M1: 只更新 deviceCookie 字段, 不回写整份配置
    ConfigManager::UpdateDeviceCookie(BuildDeviceCookie());
}
bool NcmClient::GetAccountId(long long& uid){
    // 双模式获取当前账号 uid: 镜像走 /user/account, 直连走 weapi /api/nuser/account/get
    uid = 0;
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        // cookie 走 POST body + X-NCM-Token 头, 不落 URL
        resp = RequestMirror(L"/user/account", "{}");
    } else {
        resp = RequestDirect(L"/api/nuser/account/get", "{}");
    }
    try{
        auto js = json::parse(resp);
        auto pickUid = [](const json& o)->long long{
            if(o.contains("account") && o["account"].is_object())
                return o["account"].value("id", 0LL);
            if(o.contains("profile") && o["profile"].is_object())
                return o["profile"].value("userId", 0LL);
            return 0;
        };
        long long u = pickUid(js);
        if(!u && js.contains("data") && js["data"].is_object()) u = pickUid(js["data"]);
        if(!u && js.contains("body") && js["body"].is_object()) u = pickUid(js["body"]);
        uid = u;
    } catch(...){}
    return uid > 0;
}
bool NcmClient::GetUserPlaylists(long long uid, std::vector<NcmPlaylist>& out, int limit){
    json j; j["uid"]=uid; j["limit"]=limit; j["offset"]=0; j["includeVideo"]=true;
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()) resp = RequestMirror(L"/user/playlist", j.dump());
    else resp = RequestDirect(L"/api/user/playlist", j.dump());
    try{
        auto js=json::parse(resp);
        json arr = json::array();
        if(js.contains("playlist")) arr = js["playlist"];
        else if(js.contains("data") && js["data"].contains("playlist")) arr = js["data"]["playlist"];
        else if(js.contains("body") && js["body"].contains("playlist")) arr = js["body"]["playlist"];
        out.clear();
        for(auto& p: arr){
            NcmPlaylist pl;
            pl.id = p.value("id", 0LL);
            pl.name = Utf8ToWide(p.value("name",""));
            pl.trackCount = p.value("trackCount", 0);
            if(p.contains("creator") && p["creator"].is_object()) pl.creator = Utf8ToWide(p["creator"].value("nickname",""));
            if(p.contains("coverImgUrl")) pl.coverUrl = Utf8ToWide(p["coverImgUrl"].get<std::string>());
            out.push_back(pl);
        }
        return true;
    } catch(...){ return false; }
}
bool NcmClient::GetPlaylistDetail(long long pid, std::vector<NcmSong>& outSongs, NcmPlaylist* info){
    outSongs.clear();
    if(info) *info = NcmPlaylist();

    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        // 镜像: /playlist/track/all 按 1000/页 offset 分页拉取 (cookie 走 POST body, 不落 URL)
        long long offset = 0;
        for(;;){
            json q;
            q["id"] = pid; q["limit"] = 1000; q["offset"] = offset;
            auto resp = RequestMirror(L"/playlist/track/all", q.dump());
            try{
                auto js = json::parse(resp);
                json songs;
                if(js.contains("songs")) songs = js["songs"];
                else if(js.contains("body") && js["body"].contains("songs")) songs = js["body"]["songs"];
                else if(js.contains("playlist") && js["playlist"].contains("tracks")) songs = js["playlist"]["tracks"];
                else break;
                if(!songs.is_array() || songs.empty()) break;
                for(auto& s : songs){ NcmSong song; if(ParseSongJson(s, song)) outSongs.push_back(song); }
                if(info && info->name.empty() && js.contains("playlist") && js["playlist"].is_object()){
                    info->id = pid;
                    if(js["playlist"].contains("name")) info->name = Utf8ToWide(js["playlist"].value("name",""));
                }
                if(songs.size() < 1000) break;   // 不足一页即拉完
                offset += 1000;
            }catch(...){ break; }
        }
        if(info) info->trackCount = (int)outSongs.size();
        return !outSongs.empty();
    }

    // 直连: v6/playlist/detail 返回前 1000 首 + 完整 trackIds,
    // 超出部分按 1000/批经 v3/song/detail 补拉
    json j; j["id"]=pid; j["n"]=100000; j["s"]=8;
    std::string resp = RequestDirect(L"/api/v6/playlist/detail", j.dump());
    try{
        auto js = json::parse(resp);
        json tracks;
        if(js.contains("songs")) tracks = js["songs"];
        else if(js.contains("body") && js["body"].contains("songs")) tracks = js["body"]["songs"];
        else if(js.contains("playlist") && js["playlist"].contains("tracks")) tracks = js["playlist"]["tracks"];
        else return false;
        if(!tracks.is_array()) return false;

        std::vector<long long> trackIds;
        if(js.contains("playlist") && js["playlist"].contains("trackIds") && js["playlist"]["trackIds"].is_array()){
            for(auto& t : js["playlist"]["trackIds"]) trackIds.push_back(t.value("id", 0LL));
        }

        for(auto& s : tracks){ NcmSong song; if(ParseSongJson(s, song)) outSongs.push_back(song); }

        // 分页补拉: 从已获取数量处继续, 避免与首批重复
        if(trackIds.size() > outSongs.size()){
            for(size_t off = outSongs.size(); off < trackIds.size(); off += 1000){
                size_t end = std::min(trackIds.size(), off + 1000);
                json c = json::array();
                for(size_t i = off; i < end; i++) c.push_back({{"id", trackIds[i]}});
                json dj; dj["c"] = c.dump();
                std::string dresp = RequestDirect(L"/api/v3/song/detail", dj.dump());
                try{
                    auto djs = json::parse(dresp);
                    json arr;
                    if(djs.contains("songs")) arr = djs["songs"];
                    else if(djs.contains("body") && djs["body"].contains("songs")) arr = djs["body"]["songs"];
                    if(arr.is_array()){
                        std::map<long long, json> byId;
                        for(auto& s : arr) byId[s.value("id", 0LL)] = s;
                        for(size_t i = off; i < end; i++){
                            auto it = byId.find(trackIds[i]);
                            if(it != byId.end()){
                                NcmSong song;
                                if(ParseSongJson(it->second, song)) outSongs.push_back(song);
                            }
                        }
                    }
                }catch(...){}
            }
        }
        if(info && js.contains("playlist") && js["playlist"].is_object()){
            info->id = pid;
            if(js["playlist"].contains("name")) info->name = Utf8ToWide(js["playlist"].value("name",""));
            info->trackCount = (int)outSongs.size();
        }
        return !outSongs.empty();
    } catch(...){ return false; }
}
bool NcmClient::GetSongUrl(long long id, std::string& outUrl, std::string& outType){
    std::string level = WideToUtf8(cfg_.quality);
    if(level.empty()) level="exhigh";
    return GetSongUrlLevel(id, level, outUrl, outType);
}
bool NcmClient::GetSongUrlLevel(long long id, const std::string& levelUtf8, std::string& outUrl, std::string& outType, std::string* reason){
    auto fail=[&](const char* r){ if(reason) *reason=r; return false; };
    const std::string level = levelUtf8.empty() ? "exhigh" : levelUtf8;
    // eapi 参数参考 go-musicfox: ids 数组 + level + encodeType + header
    std::string ids = "[" + std::to_string(id) + "]";
    // encodeType 是 eapi 必填参数(缺失会返回 400 参数错误);
    // 低三档用 mp3(320k, 不再强制 aac), 无损及以上用 flac, 由上游按此返回容器
    std::string encodeType = (level=="standard"||level=="higher"||level=="exhigh") ? "mp3" : "flac";
    json j;
    j["ids"] = json::parse(ids);
    j["level"] = level;
    j["encodeType"] = encodeType;
    if(level=="sky") j["immerseType"]="c51";
    // eapi 需要 header 字段（参考 go-musicfox CreateRequest）
    json header;
    EnsureDeviceCookie();   // 确保使用持久化的设备指纹
    header["osver"]="10.0.26100";
    std::string devId = CookieGet(deviceId_, "deviceId");
    header["deviceId"] = devId.empty() ? NcmCrypto::RandomString(32) : devId;
    header["appver"]="2.7.1.198277";
    header["versioncode"]="140";
    header["mobilename"]="";
    header["buildver"]=std::to_string(time(nullptr));
    header["resolution"]="1920x1080";
    header["__csrf"]="";
    header["os"]="pc";
    header["channel"]="";
    // M5: requestId 的随机部分不再用 rand()(全局状态), 改用线程局部 mt19937
    static thread_local std::mt19937 s_rng{std::random_device{}()};
    std::uniform_int_distribution<int> s_dist(0, 999);
    header["requestId"]=std::to_string(time(nullptr)*1000) + std::to_string(s_dist(s_rng));
    if(cfg_.cookie.find(L"MUSIC_U=")!=std::wstring::npos){
        // 只取 MUSIC_U= 的值, 不能把整条 cookie 串塞进 header
        size_t p = cfg_.cookie.find(L"MUSIC_U=") + 8;
        size_t e = cfg_.cookie.find(L';', p);
        header["MUSIC_U"] = WideToUtf8(cfg_.cookie.substr(p, e==std::wstring::npos?std::wstring::npos:e-p));
    }
    j["header"] = header;

    std::string resp;
    int httpStatus = 0;   // 诊断: 记录最近一次 HTTP 状态
    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        // cookie 走 POST body + X-NCM-Token 头, 不落 URL
        json q;
        q["id"] = id; q["level"] = level;
        resp = RequestMirror(L"/song/url/v1", q.dump(), true, &httpStatus);
    } else {
        resp = RequestDirect(L"/api/song/enhance/player/url/v1", j.dump(), true);
    }
    try{
        auto js=json::parse(resp);
        json data;
        if(js.contains("data") && js["data"].is_array() && !js["data"].empty()) data=js["data"][0];
        else if(js.contains("body") && js["body"].contains("data") && !js["body"]["data"].empty()) data=js["body"]["data"][0];
        else return fail("no-data");
        outUrl = data.value("url","");
        outType = data.value("type","mp3");
        int code = data.value("code",0);
        if(outUrl.empty()) return fail(("url-empty code="+std::to_string(code)).c_str());
        if(code!=200) return fail(("code="+std::to_string(code)).c_str());
        return true;
    } catch(...){
        // 诊断: 带 HTTP 状态与响应体前缀, 便于区分风控 HTML / 空响应 / 加密格式变化
        std::string clean;
        clean.reserve(resp.size());
        for(char ch : resp){
            if(ch == '\r' || ch == '\n') clean += ' ';
            else if((unsigned char)ch < 0x20) continue;
            else clean += ch;
        }
        if(clean.size() > 96) clean.resize(96);
        return fail(("json-parse-error http=" + std::to_string(httpStatus) +
                     " size=" + std::to_string(resp.size()) + " head=" + clean).c_str());
    }
}
bool NcmClient::GetSongDetail(long long id, NcmSong& out){
    json j; j["c"]= std::string("[{\"id\":")+std::to_string(id)+"}]";
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()) resp = RequestMirror(L"/song/detail", j.dump());
    else resp = RequestDirect(L"/api/v3/song/detail", j.dump());
    try{
        auto js=json::parse(resp);
        json arr;
        if(js.contains("songs")) arr=js["songs"];
        else if(js.contains("body") && js["body"].contains("songs")) arr=js["body"]["songs"];
        else return false;
        if(arr.empty()) return false;
        auto s=arr[0];
        out.id=id;
        out.title=Utf8ToWide(s.value("name",""));
        out.durationMs=s.value("dt",0);
        if(s.contains("ar") && !s["ar"].empty()){
            std::wstring arts;
            for(size_t i=0;i<s["ar"].size();++i){ if(i) arts+=L"/"; arts+=Utf8ToWide(s["ar"][i].value("name","")); }
            out.artist=arts;
        }
        if(s.contains("al")){
            out.album=Utf8ToWide(s["al"].value("name",""));
            if(s["al"].contains("picUrl")) out.coverUrl=Utf8ToWide(s["al"].value("picUrl",""));
        }
        return true;
    } catch(...){ return false; }
}
bool NcmClient::GetLyric(long long id, std::string& outLrc){
    json j; j["id"]=id; j["lv"]=1; j["tv"]=1;
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()) resp = RequestMirror(L"/lyric", j.dump());
    else resp = RequestDirect(L"/api/song/lyric", j.dump());
    try{
        auto js=json::parse(resp);
        std::string orig  = js.contains("lrc")    ? js["lrc"].value("lyric","")    : "";
        std::string trans = js.contains("tlyric") ? js["tlyric"].value("lyric","") : "";
        // 优先合并中文翻译(tlyric); klyric/yrc 逐字歌词暂不使用
        outLrc = MergeLyricWithTranslation(orig, trans);
        return true;
    } catch(...){ return false; }
}
bool NcmClient::IsLoggedIn(){ return !cfg_.cookie.empty() && cfg_.cookie.find(L"MUSIC_U")!=std::wstring::npos; }
long long NcmClient::ParseUidFromCookie(const std::wstring& cookie){
    // MUSIC_U 是 base64 编码的 JSON: {"uid":xxx,...}
    size_t p = cookie.find(L"MUSIC_U=");
    if(p==std::wstring::npos) return 0;
    p += 8;
    size_t e = cookie.find(L';', p);
    std::string b64 = WideToUtf8(cookie.substr(p, e==std::wstring::npos?std::wstring::npos:e-p));
    // 去掉可能的空白
    while(!b64.empty() && (b64.back()==' '||b64.back()=='\t'||b64.back()=='\r'||b64.back()=='\n')) b64.pop_back();
    // base64 解码（注意: CRYPT_STRING_NOCRLF 只用于编码; 解码用 CRYPT_STRING_BASE64_ANY）
    DWORD len=0;
    if(!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64_ANY, nullptr, &len, nullptr, nullptr)) return 0;
    std::string dec(len,0);
    if(!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64_ANY, (BYTE*)dec.data(), &len, nullptr, nullptr)) return 0;
    dec.resize(len);
    try{
        auto j = json::parse(dec);
        if(j.contains("uid")) return j["uid"].get<long long>();
    }catch(...){}
    return 0;
}

