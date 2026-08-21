#include "ncm_client.h"
#include "http_client.h"
#include "ncm_crypto.h"
#include "utils.h"
#include "../third_party/nlohmann/json.hpp"
#include <ctime>
#include <cstdlib>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
using json = nlohmann::json;

// ---- 设备指纹工具（参考 go-musicfox/netease-music） ----

// 生成 52 位随机 hex 的 sDeviceId
static std::string GenSDeviceId(){
    std::string out;
    out.reserve(52);
    const char* hex="0123456789ABCDEF";
    for(int i=0;i<52;i++) out += hex[rand()%16];
    return out;
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

std::string NcmClient::DoPost(const std::wstring& url, const std::string& body, const std::wstring& extraHeaders){
    std::wstring cookie = cfg_.cookie;
    auto r = HttpClient::Post(url, body, extraHeaders, cookie);
    return r.body;
}
std::string NcmClient::RequestMirror(const std::wstring& path, const std::string& jsonData, bool post){
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
    auto resp = HttpClient::Post(url, body, L"", L"");
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
    // 初始化/补全设备 Cookie（持久化在成员里，避免每次随机）
    // 参考 NeteaseCloudMusicApiEnhanced processCookieObject + go-musicfox
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
        std::string rnd;
        const char* alpha="abcdefghijklmnopqrstuvwxyz";
        for(int i=0;i<6;i++) rnd += alpha[rand()%26];
        deviceId_ += L"; WNMCID=" + Utf8ToWide(rnd + "." + std::to_string(time(nullptr)*1000) + ".01.0");
    }
}
bool NcmClient::QrCreate(QrLogin& out){
    // type=3 是当前有效协议（参考 NeteaseCloudMusicApiEnhanced，最新维护）
    std::string jsonData = "{\"type\":3}";
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()) resp = RequestMirror(L"/login/qr/key", jsonData);
    else resp = RequestDirect(L"/api/login/qrcode/unikey", jsonData);
    try {
        auto j = json::parse(resp);
        std::string key;
        if(j.contains("data") && j["data"].contains("unikey")) key = j["data"]["unikey"].get<std::string>();
        else if(j.contains("unikey")) key = j["unikey"].get<std::string>();
        else if(j.contains("data") && j["data"].contains("data") && j["data"]["data"].contains("unikey")) key = j["data"]["data"]["unikey"].get<std::string>();
        if(key.empty()) return false;
        out.unikey=key;
        // 生成 chainId（新协议要求，否则手机端提示"设备环境异常"）
        // chainId = v1_<sDeviceId>_web_login_<timestamp_ms>  (参考 go-musicfox)
        EnsureDeviceCookie();
        std::string sdev = CookieGet(deviceId_, "sDeviceId");
        if(sdev.empty()){
            sdev = GenSDeviceId();
            deviceId_ += L"; sDeviceId=" + Utf8ToWide(sdev);
        }
        long long ms = (long long)time(nullptr) * 1000;
        std::string chainId = "v1_" + sdev + "_web_login_" + std::to_string(ms);
        // 注意: 官方用 http:// 而非 https://
        out.qrUrl = L"http://music.163.com/login?codekey=" + Utf8ToWide(key) + L"&chainId=" + Utf8ToWide(chainId);
        return true;
    } catch(...){ return false; }
}
int NcmClient::QrCheck(const std::string& key, std::string& outCookie, std::wstring& outMsg, std::wstring* outChallengeUrl, long long* outUid){
    json j; j["key"]=key; j["type"]=3;
    std::string resp;
    std::map<std::string,std::string> setCookies;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        resp = RequestMirror(L"/login/qr/check", j.dump());
    } else {
        // 直连: 需要拿 Set-Cookie 头（登录态在响应头里）
        auto w = NcmCrypto::Weapi(j.dump());
        std::wstring wparams = Utf8ToWide(w.params);
        std::string encParams = HttpClient::UrlEncodeW(wparams);
        std::string body = "params=" + encParams + "&encSecKey=" + w.encSecKey;
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
        auto r = HttpClient::Post(L"https://music.163.com/weapi/login/qrcode/client/login", body, headers, cookie);
        resp = r.body;
        setCookies = r.cookies;
    }
    try{
        auto js = json::parse(resp);
        int code = js.value("code", 0);
        if(js.contains("data") && js["data"].is_object() && js["data"].contains("code")) code = js["data"].value("code", code);
        // 提取风控验证跳转地址(滑块challenge): 顶层/data/body 均可能携带
        auto pickRedirect = [](const json& o)->std::string{
            for(const char* k : {"redirectUrl","redirect_url","verifyUrl","captchaUrl"}){
                if(o.contains(k) && o[k].is_string()){
                    auto v = o[k].get<std::string>();
                    if(!v.empty()) return v;
                }
            }
            return "";
        };
        std::string redirect = pickRedirect(js);
        if(redirect.empty() && js.contains("data") && js["data"].is_object()) redirect = pickRedirect(js["data"]);
        if(redirect.empty() && js.contains("body") && js["body"].is_object()) redirect = pickRedirect(js["body"]);
        if(outChallengeUrl) *outChallengeUrl = Utf8ToWide(redirect);
        if(code==803){
            // 优先从 Set-Cookie 头收集完整登录态（MUSIC_U 等）
            std::string sc;
            for(auto& kv : setCookies){
                if(!sc.empty()) sc += "; ";
                sc += kv.first + "=" + kv.second;
            }
            if(!sc.empty()){
                outCookie = sc;
            } else if(js.contains("cookie")) outCookie = js["cookie"].get<std::string>();
            else if(js.contains("data") && js["data"].contains("cookie")) outCookie = js["data"]["cookie"].get<std::string>();
            outMsg = L"login success";
            if(outUid){
                // 803 响应体自带账号信息, 直接提取 uid (MUSIC_U 已无法解析出 uid)
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
                *outUid = u;
            }
            return 803;
        } else if(code==802){ outMsg=L"待确认，请在手机上点击确认"; return 802; }
        else if(code==801){ outMsg=L"等待扫码"; return 801; }
        else if(code==800){ outMsg=L"二维码已过期，请重新获取"; return 800; }
        else if(code==8821){ outMsg=L"需要行为验证（请在手机上完成验证后重新扫码）"; return 8821; }
        else if(code==8822){ outMsg=L"需要验证码，请在手机上查看"; return 8822; }
        return code;
    } catch(...){ return -1; }
}
bool NcmClient::GetAccountId(long long& uid){
    // 双模式获取当前账号 uid: 镜像走 /user/account, 直连走 weapi /api/nuser/account/get
    uid = 0;
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        std::wstring base = cfg_.apiUrl;
        if(!base.empty() && base.back()==L'/') base.pop_back();
        std::wstring url = base + L"/user/account";
        if(!cfg_.cookie.empty()) url += L"?cookie=" + Utf8ToWide(HttpClient::UrlEncodeW(cfg_.cookie));
        resp = HttpClient::Get(url).body;
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
    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        std::wstring url = cfg_.apiUrl + L"/playlist/track/all?id=" + std::to_wstring(pid) + L"&limit=1000";
        if(!cfg_.cookie.empty()) url += L"&cookie=" + Utf8ToWide(HttpClient::UrlEncodeW(cfg_.cookie));
        auto r = HttpClient::Get(url);
        resp = r.body;
    } else {
        json j; j["id"]=pid; j["n"]=100000; j["s"]=8;
        resp = RequestDirect(L"/api/v6/playlist/detail", j.dump());
    }
    try{
        auto js=json::parse(resp);
        json songs;
        if(js.contains("songs")) songs=js["songs"];
        else if(js.contains("body") && js["body"].contains("songs")) songs=js["body"]["songs"];
        else if(js.contains("playlist") && js["playlist"].contains("tracks")) songs=js["playlist"]["tracks"];
        else return false;
        outSongs.clear();
        for(auto& s: songs){
            NcmSong song;
            song.id = s.value("id",0LL);
            song.title = Utf8ToWide(s.value("name",""));
            song.durationMs = s.value("dt",0);
            if(s.contains("ar") && s["ar"].is_array() && !s["ar"].empty()){
                std::wstring arts;
                for(size_t i=0;i<s["ar"].size();++i){
                    if(i) arts+=L"/";
                    arts+=Utf8ToWide(s["ar"][i].value("name",""));
                }
                song.artist=arts;
            }
            if(s.contains("al")){
                song.album=Utf8ToWide(s["al"].value("name",""));
                if(s["al"].contains("picUrl")) song.coverUrl=Utf8ToWide(s["al"].value("picUrl",""));
            }
            outSongs.push_back(song);
        }
        if(info && js.contains("playlist")){
            auto p=js["playlist"];
            info->id=pid;
            if(p.contains("name")) info->name=Utf8ToWide(p.value("name",""));
            info->trackCount=(int)outSongs.size();
        }
        return true;
    } catch(...){ return false; }
}
bool NcmClient::GetSongUrl(long long id, std::string& outUrl, std::string& outType){
    std::string level = WideToUtf8(cfg_.quality);
    if(level.empty()) level="exhigh";
    // eapi 参数参考 go-musicfox: ids 数组 + level + encodeType + header
    std::string ids = "[" + std::to_string(id) + "]";
    std::string encodeType = (level=="standard"||level=="higher"||level=="exhigh") ? "aac" : "flac";
    json j;
    j["ids"] = json::parse(ids);
    j["level"] = level;
    j["encodeType"] = encodeType;
    if(level=="sky") j["immerseType"]="c51";
    // eapi 需要 header 字段（参考 go-musicfox CreateRequest）
    json header;
    header["osver"]="10.0.26100";
    header["deviceId"]=NcmCrypto::RandomString(32);
    header["appver"]="2.7.1.198277";
    header["versioncode"]="140";
    header["mobilename"]="";
    header["buildver"]=std::to_string(time(nullptr));
    header["resolution"]="1920x1080";
    header["__csrf"]="";
    header["os"]="pc";
    header["channel"]="";
    header["requestId"]=std::to_string(time(nullptr)*1000) + std::to_string(rand()%1000);
    if(cfg_.cookie.find(L"MUSIC_U=")!=std::wstring::npos){
        // 只取 MUSIC_U= 的值, 不能把整条 cookie 串塞进 header
        size_t p = cfg_.cookie.find(L"MUSIC_U=") + 8;
        size_t e = cfg_.cookie.find(L';', p);
        header["MUSIC_U"] = WideToUtf8(cfg_.cookie.substr(p, e==std::wstring::npos?std::wstring::npos:e-p));
    }
    j["header"] = header;

    std::string resp;
    if(cfg_.useProxy && !cfg_.apiUrl.empty()){
        std::wstring url = cfg_.apiUrl + L"/song/url/v1?id=" + std::to_wstring(id) + L"&level=" + Utf8ToWide(level);
        if(!cfg_.cookie.empty()) url += L"&cookie=" + Utf8ToWide(HttpClient::UrlEncodeW(cfg_.cookie));
        auto r=HttpClient::Get(url);
        resp=r.body;
    } else {
        resp = RequestDirect(L"/api/song/enhance/player/url/v1", j.dump(), true);
    }
    try{
        auto js=json::parse(resp);
        json data;
        if(js.contains("data") && js["data"].is_array() && !js["data"].empty()) data=js["data"][0];
        else if(js.contains("body") && js["body"].contains("data") && !js["body"]["data"].empty()) data=js["body"]["data"][0];
        else return false;
        outUrl = data.value("url","");
        outType = data.value("type","mp3");
        int code = data.value("code",0);
        if(outUrl.empty() || code!=200) return false;
        return true;
    } catch(...){ return false; }
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
        if(js.contains("lrc") && js["lrc"].contains("lyric")) outLrc=js["lrc"].value("lyric","");
        else outLrc="";
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

