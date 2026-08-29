#include "meta_cache.h"
#include "utils.h"
#include "error_notify.h"
#include <cstring>
#include <fstream>
#include <winhttp.h>
#include <mutex>
#include <map>
#include "../third_party/nlohmann/json.hpp"
#pragma comment(lib, "winhttp.lib")
using json = nlohmann::json;

void FsLog(const char* what);

namespace {

std::wstring ArtCacheDir(){
    WCHAR tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"aimp_ncm\\artwork";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring LyricCacheDir(){
    WCHAR tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"aimp_ncm\\lyric";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

bool ReadFileAll(const std::wstring& path, std::string& out){
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if(f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li; GetFileSizeEx(f, &li);
    out.assign((size_t)li.QuadPart, 0);
    DWORD got = 0, total = 0;
    while(total < out.size()){
        if(!ReadFile(f, out.data() + total, (DWORD)(out.size() - total), &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(f);
    out.resize(total);
    return !out.empty();
}

void WriteFileAll(const std::wstring& path, const std::string& bytes){
    // D4: 先写 .tmp 再原子替换, 避免读者看到半截文件
    std::wstring tmp = path + L".tmp";
    HANDLE w = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if(w != INVALID_HANDLE_VALUE){
        DWORD written = 0;
        WriteFile(w, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
        CloseHandle(w);
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

// WinHTTP GET -> 内存字节
bool HttpDownloadBytes(const std::wstring& url, std::string& out){
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
    WCHAR host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    if(!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;

    HINTERNET ses = WinHttpOpen(L"AIMP-NCM/1.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if(!ses) return false;
    DWORD t = 10000;
    WinHttpSetOption(ses, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    WinHttpSetOption(ses, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    HINTERNET con = WinHttpConnect(ses, uc.lpszHostName, uc.nPort, 0);
    HINTERNET req = nullptr;
    bool ok = false;
    if(con){
        bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
        req = WinHttpOpenRequest(con, L"GET", uc.lpszUrlPath, nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 https ? WINHTTP_FLAG_SECURE : 0);
        if(req){
            DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                             SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
            WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
            if(SUCCEEDED(WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) &&
               WinHttpReceiveResponse(req, nullptr)){
                DWORD status = 0, statusLen = sizeof(status);
                if(WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                       nullptr, &status, &statusLen, nullptr))
                    NcmErrorNotifyAccess((int)status);
                for(;;){
                    DWORD avail = 0;
                    if(!WinHttpQueryDataAvailable(req, &avail) || avail == 0) { ok = true; break; }
                    size_t before = out.size();
                    out.resize(before + avail);
                    DWORD got = 0;
                    if(!WinHttpReadData(req, out.data() + before, avail, &got)){ ok = false; break; }
                    out.resize(before + got);
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok && !out.empty();
}

// 网易云 al.picUrl 可能是协议相对地址 (//p1.music.126.net/...)，下载前补全
inline std::wstring NormalizeCoverUrl(const std::wstring& url){
    if(url.rfind(L"//", 0) == 0) return L"https:" + url;
    return url;
}

// ---- 元数据缓存: %TEMP%\aimp_ncm\song_meta.json ----
// 结构: { "<pid>": { "<tid>": {title, artist, album, durationMs, coverUrl} } }
std::wstring MetaCachePath(){
    WCHAR tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"aimp_ncm";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\song_meta.json";
}

struct MetaDb { std::map<long long, std::map<long long, NcmSong>> byPid; unsigned __int64 mtime = 0; };
MetaDb g_meta;
std::mutex g_metaMtx;

unsigned __int64 FileMTime(const std::wstring& path){
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if(!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (((unsigned __int64)d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

// 调用方需持有 g_metaMtx
void LoadMetaFile(){
    g_meta.byPid.clear();
    std::ifstream f(WideToUtf8(MetaCachePath()));
    if(!f) return;
    try{
        json j; f >> j;
        for(auto it = j.begin(); it != j.end(); ++it){
            long long pid = std::stoll(it.key());
            auto& m = g_meta.byPid[pid];
            for(auto jt = it.value().begin(); jt != it.value().end(); ++jt){
                long long tid = std::stoll(jt.key());
                NcmSong s;
                s.id = tid;
                s.title = Utf8ToWide(jt.value().value("title", ""));
                s.artist = Utf8ToWide(jt.value().value("artist", ""));
                s.album = Utf8ToWide(jt.value().value("album", ""));
                s.durationMs = jt.value().value("durationMs", 0);
                s.coverUrl = Utf8ToWide(jt.value().value("coverUrl", ""));
                m[tid] = s;
            }
        }
    }catch(...){ g_meta.byPid.clear(); }
}

// 调用方需持有 g_metaMtx
void SaveMetaFile(){
    json j = json::object();
    for(auto& kv : g_meta.byPid){
        json inner = json::object();
        for(auto& sv : kv.second){
            json o;
            o["title"] = WideToUtf8(sv.second.title);
            o["artist"] = WideToUtf8(sv.second.artist);
            o["album"] = WideToUtf8(sv.second.album);
            o["durationMs"] = sv.second.durationMs;
            o["coverUrl"] = WideToUtf8(sv.second.coverUrl);
            inner[std::to_string(sv.first)] = o;
        }
        j[std::to_string(kv.first)] = inner;
    }
    // D4: 原子写(临时文件 + 替换), 避免多进程/多线程读到半截 JSON
    std::wstring path = MetaCachePath();
    std::wstring tmp = path + L".tmp";
    std::ofstream f(WideToUtf8(tmp), std::ios::trunc);
    if(f){
        f << j.dump();
        f.close();
        MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    g_meta.mtime = FileMTime(MetaCachePath());
}

// 高频读路径: 仅当外部(GUI/同步)改写文件后重载 (调用方需持有 g_metaMtx 时使用此函数)
void ReloadMetaIfChanged(){
    unsigned __int64 mt = FileMTime(MetaCachePath());
    if(mt != g_meta.mtime){ g_meta.mtime = mt; LoadMetaFile(); }
}

const NcmSong* LookupLocked(long long pid, long long tid){
    ReloadMetaIfChanged();
    auto p = g_meta.byPid.find(pid);
    if(p != g_meta.byPid.end()){
        auto s = p->second.find(tid);
        if(s != p->second.end()) return &s->second;
    }
    return nullptr;
}

const NcmSong* LookupByTidLocked(long long tid){
    ReloadMetaIfChanged();
    for(auto& p : g_meta.byPid){
        auto s = p.second.find(tid);
        if(s != p.second.end()) return &s->second;
    }
    return nullptr;
}

// ---- ID3v2.3 标签构建 ----

void AppendSynchsafe(std::string& out, unsigned v){
    out.push_back((char)((v >> 21) & 0x7F));
    out.push_back((char)((v >> 14) & 0x7F));
    out.push_back((char)((v >> 7) & 0x7F));
    out.push_back((char)(v & 0x7F));
}

void AppendFrameHead(std::string& out, const char id[4], unsigned size){
    out.append(id, 4);
    out.push_back((char)((size >> 24) & 0xFF));
    out.push_back((char)((size >> 16) & 0xFF));
    out.push_back((char)((size >> 8) & 0xFF));
    out.push_back((char)(size & 0xFF));
    out.push_back(0); out.push_back(0);   // frame flags
}

// 文本帧: UTF-16 with BOM (0x01)
void AppendTextFrame(std::string& out, const char id[4], const std::wstring& text){
    if(text.empty()) return;
    std::string content;
    content.push_back(0x01);
    content.push_back((char)0xFF); content.push_back((char)0xFE);   // BOM
    for(wchar_t c : text){
        content.push_back((char)(c & 0xFF));
        content.push_back((char)((c >> 8) & 0xFF));
    }
    AppendFrameHead(out, id, (unsigned)content.size());
    out += content;
}

// 依据文件头 magic 识别图片 MIME (JPEG/PNG/GIF/BMP/WebP), 未知回退 image/jpeg
std::string DetectImageMime(const std::string& data){
    if(data.size() >= 3 && (unsigned char)data[0]==0xFF && (unsigned char)data[1]==0xD8 && (unsigned char)data[2]==0xFF)
        return "image/jpeg";
    static const char PNG_MAGIC[8] = {(char)0x89,'P','N','G','\r','\n',0x1A,'\n'};
    if(data.size() >= 8 && memcmp(data.data(), PNG_MAGIC, 8) == 0)
        return "image/png";
    if(data.size() >= 6 && data.compare(0, 4, "GIF8") == 0)
        return "image/gif";
    if(data.size() >= 2 && data[0]=='B' && data[1]=='M')
        return "image/bmp";
    if(data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    return "image/jpeg";
}

// 封面帧 APIC: encoding=0, mime=magic 检测, pictype=3 (front cover), 空描述
void AppendApicFrame(std::string& out, const std::string& img){
    if(img.empty()) return;
    std::string content;
    content.push_back(0x00);                       // text encoding: latin1
    content += DetectImageMime(img);
    content.push_back(0x00);                       // mime terminator
    content.push_back(0x03);                       // picture type: cover (front)
    content.push_back(0x00);                       // description: empty
    content += img;
    AppendFrameHead(out, "APIC", (unsigned)content.size());
    out += content;
}

// 歌词帧 USLT: encoding=1 (UTF-16 with BOM), language=eng, 空描述
void AppendUsltFrame(std::string& out, const std::string& lrcUtf8){
    if(lrcUtf8.empty()) return;
    std::wstring w = Utf8ToWide(lrcUtf8);
    std::string content;
    content.push_back(0x01);                       // text encoding: UTF-16
    content += "eng";                              // ISO-639-2 language
    content.push_back(0x00);                       // content descriptor: empty
    content.push_back((char)0xFF); content.push_back((char)0xFE);   // BOM
    for(wchar_t c : w){
        content.push_back((char)(c & 0xFF));
        content.push_back((char)((c >> 8) & 0xFF));
    }
    AppendFrameHead(out, "USLT", (unsigned)content.size());
    out += content;
}

} // namespace

namespace NcmMeta {

void WritePlaylist(long long pid, const std::vector<NcmSong>& songs){
    std::lock_guard<std::mutex> lk(g_metaMtx);
    ReloadMetaIfChanged();  // 载入既有数据(GUI/其他歌单), 避免覆盖
    auto& m = g_meta.byPid[pid];
    for(auto& s : songs)
        if(s.id > 0) m[s.id] = s;
    SaveMetaFile();
}

bool Lookup(long long pid, long long tid, NcmSong& out){
    std::lock_guard<std::mutex> lk(g_metaMtx);
    const NcmSong* s = LookupLocked(pid, tid);
    if(!s) s = LookupByTidLocked(tid);
    if(s){ out = *s; return true; }
    return false;
}

bool LookupByTid(long long tid, NcmSong& out){
    std::lock_guard<std::mutex> lk(g_metaMtx);
    const NcmSong* s = LookupByTidLocked(tid);
    if(s){ out = *s; return true; }
    return false;
}

void Upsert(long long pid, const NcmSong& song){
    std::lock_guard<std::mutex> lk(g_metaMtx);
    g_meta.byPid[pid][song.id] = song;
    SaveMetaFile();
}

bool GetCoverBytes(long long tid, const std::wstring& coverUrl, std::string& out){
    if(tid <= 0) return false;
    std::wstring p = ArtCacheDir() + L"\\" + std::to_wstring(tid) + L".img";
    if(ReadFileAll(p, out)) return true;
    if(coverUrl.empty()) return false;
    std::wstring url = NormalizeCoverUrl(coverUrl);
    // 封面以源头原始尺寸下载, 不再强制追加 ?param=500y500 缩略参数
    if(!HttpDownloadBytes(url, out)) return false;
    if(!out.empty()) WriteFileAll(p, out);
    return !out.empty();
}

bool GetLyricText(long long tid, std::string& out){
    if(tid <= 0) return false;
    std::wstring p = LyricCacheDir() + L"\\" + std::to_wstring(tid) + L".lrc";
    if(ReadFileAll(p, out)) return true;
    // 未命中: 网络拉取(直连 weapi /api/song/lyric 或镜像 /lyric), 成功后落盘
    // 注意: 本函数在锁外调用, 可安全进行网络请求
    NcmConfig cfg; ConfigManager::Load(cfg);
    NcmClient client(cfg);
    if(!client.GetLyric(tid, out) || out.empty()) return false;
    WriteFileAll(p, out);
    return true;
}

std::string BuildStreamTag(long long pid, long long tid){
    NcmSong song;
    {
        // 只在锁内拷贝元数据; 封面/歌词等网络操作一律放到锁外
        std::lock_guard<std::mutex> lk(g_metaMtx);
        const NcmSong* s = LookupLocked(pid, tid);
        if(!s) s = LookupByTidLocked(tid);
        if(!s) return "";
        song = *s;
    }

    std::string frames;
    AppendTextFrame(frames, "TIT2", song.title);
    AppendTextFrame(frames, "TPE1", song.artist);
    AppendTextFrame(frames, "TALB", song.album);
    // 不注入 TLEN: 元数据时长与实际流时长可能不符, 让 AIMP 按流实测
    // 封面: 磁盘缓存优先, 未命中下载(锁外)
    if(!song.coverUrl.empty()){
        std::string cover;
        if(GetCoverBytes(tid, song.coverUrl, cover)) AppendApicFrame(frames, cover);
    }
    // 歌词: 磁盘缓存优先, 未命中拉取(锁外); 失败则不发歌词帧, 不影响播放
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(cfg.lyricMode == L"uslt"){
        std::string lrc;
        if(GetLyricText(tid, lrc)) AppendUsltFrame(frames, lrc);
    }
    if(frames.empty()) return "";

    std::string tag;
    tag.append("ID3", 3);
    tag.push_back(3); tag.push_back(0); tag.push_back(0);   // v2.3.0, no flags
    AppendSynchsafe(tag, (unsigned)frames.size());
    tag += frames;
    return tag;
}

} // namespace NcmMeta
