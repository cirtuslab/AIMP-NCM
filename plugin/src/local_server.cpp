// winsock2 must be included before windows.h (pulled in by config.h)
#include <winsock2.h>
#include <ws2tcpip.h>
#include "local_server.h"
#include "config.h"
#include "ncm_client.h"
#include "utils.h"
#include <winhttp.h>
#include <atomic>
#include <thread>
#include <string>
#include <fstream>
#include <algorithm>
#include <set>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

void FsLog(const char* what);

namespace {

std::atomic<bool> g_run{false};
SOCKET g_listen = INVALID_SOCKET;
std::thread g_acceptThread;

std::wstring CacheDir(){
    WCHAR tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"aimp_ncm\\cache";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void CleanupOldCache(){
    // 策略: cfg.cacheDays<=0 表示永不自动删除; 白名单歌单(pid)的缓存跳过
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(cfg.cacheDays < 0) return;

    std::set<long long> whitelist;
    {
        const wchar_t* s = cfg.cacheWhitelist.c_str();
        while(*s){
            if(iswdigit(*s)){
                long long v = 0;
                while(iswdigit(*s)){ v = v*10 + (*s - L'0'); ++s; }
                whitelist.insert(v);
            } else ++s;
        }
    }

    const ULONGLONG maxAgeMs = (cfg.cacheDays > 0 ? (ULONGLONG)cfg.cacheDays : 7ull)
                               * 24ull * 3600ull * 1000ull;
    WIN32_FIND_DATAW fd;
    std::wstring pat = CacheDir() + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if(h == INVALID_HANDLE_VALUE) return;
    do{
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // 文件名: {pid}_{tid}.{ext}; 旧格式 {tid}.{ext} 无歌单归属, 不受白名单保护
        std::wstring name = fd.cFileName;
        size_t under = name.find(L'_');
        long long pid = -1;
        bool hasPid = false;
        if(under != std::wstring::npos && under > 0 && iswdigit(name[0])){
            try{ pid = std::stoll(name.substr(0, under)); hasPid = true; }catch(...){}
        }
        if(hasPid && whitelist.count(pid)) continue;   // 白名单歌单缓存永不删除

        std::wstring full = CacheDir() + L"\\" + name;
        HANDLE f = CreateFileW(full.c_str(), GENERIC_READ,
                               FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if(f == INVALID_HANDLE_VALUE) continue;
        FILETIME wt; GetFileTime(f, nullptr, nullptr, &wt);
        CloseHandle(f);
        ULARGE_INTEGER u; u.LowPart = wt.dwLowDateTime; u.HighPart = wt.dwHighDateTime;
        ULONGLONG ms = u.QuadPart / 10000;
        if(GetTickCount64() - ms > maxAgeMs) DeleteFileW(full.c_str());
    }while(FindNextFileW(h, &fd));
    FindClose(h);
}

const wchar_t* MimeFor(const std::wstring& ext){
    if(ext == L"flac") return L"audio/flac";
    if(ext == L"m4a")  return L"audio/mp4";
    if(ext == L"wav")  return L"audio/wav";
    if(ext == L"ape")  return L"audio/x-ape";
    if(ext == L"ogg")  return L"audio/ogg";
    return L"audio/mpeg"; // mp3 and default
}

bool SendAll(SOCKET s, const char* data, int len){
    int sent = 0;
    while(sent < len){
        int w = send(s, data + sent, len - sent, 0);
        if(w <= 0) return false;
        sent += w;
    }
    return true;
}
bool SendAll(SOCKET s, const std::string& data){ return SendAll(s, data.c_str(), (int)data.size()); }

long long ParseRangeStart(const std::string& req);

// serve a local file directly (supports Range); returns whether handled
bool ServeFile(SOCKET s, const std::wstring& path, const std::wstring& ext, long long rangeStart){
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if(f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if(!GetFileSizeEx(f, &li)){ CloseHandle(f); return false; }
    long long size = li.QuadPart;
    if(rangeStart >= size){ CloseHandle(f); return false; } // invalid range -> treat as miss
    bool partial = rangeStart > 0;

    std::string head = partial
        ? "HTTP/1.1 206 Partial Content\r\n"
          "Content-Range: bytes " + std::to_string(rangeStart) + "-" + std::to_string(size-1) + "/" + std::to_string(size) + "\r\n"
          "Content-Length: " + std::to_string(size - rangeStart) + "\r\n"
        : "HTTP/1.1 200 OK\r\n"
          "Content-Length: " + std::to_string(size) + "\r\n";
    head += std::string("Content-Type: ") + WideToUtf8(MimeFor(ext)) +
            "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
    if(!SendAll(s, head)){ CloseHandle(f); shutdown(s, SD_SEND); closesocket(s); return true; }

    LARGE_INTEGER pos; pos.QuadPart = rangeStart;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    char buf[256*1024];   // per-connection thread: keep on stack, not static
    long long remaining = size - rangeStart;
    while(remaining > 0){
        DWORD want = (DWORD)std::min<long long>(sizeof(buf), remaining), got = 0;
        if(!ReadFile(f, buf, want, &got, nullptr) || got == 0) break;
        if(!SendAll(s, buf, (int)got)) break;
        remaining -= got;
    }
    CloseHandle(f);
    shutdown(s, SD_SEND);
    closesocket(s);
    return true;
}

// cache hit: 查找 {pid}_{tid}.* (.part 未完成文件除外)
bool TryServeCached(SOCKET s, long long pid, long long tid, long long rangeStart){
    WIN32_FIND_DATAW fd;
    std::wstring pat = CacheDir() + L"\\" + std::to_wstring(pid) + L"_" +
                       std::to_wstring(tid) + L".*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if(h == INVALID_HANDLE_VALUE) return false;
    std::wstring name = fd.cFileName;
    FindClose(h);
    if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    size_t dot = name.find_last_of(L'.');
    if(dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot + 1);
    if(ext == L"part") return false;                       // do not serve partials
    if(fd.nFileSizeLow == 0 && fd.nFileSizeHigh == 0) return false;

    FsLog(("cache hit tid=" + std::to_string(tid)).c_str());
    return ServeFile(s, CacheDir() + L"\\" + name, ext, rangeStart);
}

// ---------------- CDN streaming via raw WinHTTP handles (pipe to socket + disk cache) ----------------
struct HttpGet {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    long long total = -1;      // Content-Length, -1 when unknown
};

void HttpClose(HttpGet& g){
    if(g.req) WinHttpCloseHandle(g.req);
    if(g.con) WinHttpCloseHandle(g.con);
    if(g.ses) WinHttpCloseHandle(g.ses);
    g = {};
}

bool HttpOpenGet(const std::wstring& url, HttpGet& g){
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
    WCHAR host[256] = {0}, path[2048] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
    if(!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;

    g.ses = WinHttpOpen(L"AIMP-NCM/1.3", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if(!g.ses) return false;
    DWORD t = 15000;
    WinHttpSetOption(g.ses, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    WinHttpSetOption(g.ses, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    WinHttpSetOption(g.ses, WINHTTP_OPTION_RESOLVE_TIMEOUT, &t, sizeof(t));

    g.con = WinHttpConnect(g.ses, uc.lpszHostName, uc.nPort, 0);
    if(!g.con){ HttpClose(g); return false; }
    bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    g.req = WinHttpOpenRequest(g.con, L"GET", uc.lpszUrlPath, nullptr,
                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               https ? WINHTTP_FLAG_SECURE : 0);
    if(!g.req){ HttpClose(g); return false; }
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(g.req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    WinHttpAddRequestHeaders(g.req,
        L"User-Agent: AIMP-NCM/1.3\r\nReferer: https://music.163.com\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if(FAILED(WinHttpSendRequest(g.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) ||
       !WinHttpReceiveResponse(g.req, nullptr)){
        HttpClose(g); return false;
    }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(g.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &status, &sz, nullptr);
    if(status != 200 && status != 206){ HttpClose(g); return false; }
    DWORD len = 0; sz = sizeof(len);
    if(WinHttpQueryHeaders(g.req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                           nullptr, &len, &sz, nullptr)) g.total = (long long)len;
    return true;
}

int HttpRead(HttpGet& g, char* buf, int len){
    DWORD avail = 0;
    if(!WinHttpQueryDataAvailable(g.req, &avail)) return -1;
    if(avail == 0) return 0;
    if(avail > (DWORD)len) avail = len;
    DWORD got = 0;
    if(!WinHttpReadData(g.req, buf, avail, &got)) return -1;
    return (int)got;
}

// pipe CDN -> socket (only bytes >= rangeStart) while caching full content to disk.
// client disconnect mid-way does NOT abort the download: next play hits the cache.
bool ProxyAndCache(SOCKET s, long long pid, long long tid, const std::wstring& url,
                   const std::wstring& ext, long long rangeStart){
    HttpGet g;
    if(!HttpOpenGet(url, g)) return false;

    long long total = g.total;
    std::string head;
    if(rangeStart > 0 && total > 0)
        head = "HTTP/1.1 206 Partial Content\r\n"
               "Content-Range: bytes " + std::to_string(rangeStart) + "-" + std::to_string(total-1) + "/" + std::to_string(total) + "\r\n"
               "Content-Length: " + std::to_string(total - rangeStart) + "\r\n";
    else if(total > 0)
        head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) + "\r\n";
    else
        head = "HTTP/1.1 200 OK\r\n";
    head += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n";
    if(!SendAll(s, head)){ HttpClose(g); shutdown(s, SD_SEND); closesocket(s); return false; }

    std::wstring partPath = CacheDir() + L"\\" + std::to_wstring(pid) + L"_" + std::to_wstring(tid) + L".part";
    std::ofstream cf(partPath.c_str(), std::ios::binary | std::ios::trunc);
    bool sendAlive = true, dlError = false;
    long long pos = 0;
    char buf[64*1024];
    while(true){
        int r = HttpRead(g, buf, sizeof(buf));
        if(r < 0){ dlError = true; break; }
        if(r == 0) break;
        if(cf.is_open()){
            cf.write(buf, r);
            if(!cf.good()){ cf.close(); DeleteFileW(partPath.c_str()); } // disk error: drop cache
        }
        if(sendAlive){
            if(pos < rangeStart){
                long long skip = std::min<long long>(r, rangeStart - pos);
                if(!SendAll(s, buf + skip, r - (int)skip)) sendAlive = false;
            } else {
                if(!SendAll(s, buf, r)) sendAlive = false;
            }
        }
        pos += r;
    }
    HttpClose(g);

    bool complete = !dlError && (total < 0 || pos >= total);
    if(complete && pos > 0){
        if(cf.is_open()) cf.close();
        std::wstring finalPath = CacheDir() + L"\\" + std::to_wstring(pid) + L"_" + std::to_wstring(tid) + L"." + ext;
        DeleteFileW(finalPath.c_str()); // MoveFile does not overwrite existing target
        if(MoveFileW(partPath.c_str(), finalPath.c_str()))
            FsLog(("cached tid=" + std::to_string(tid)).c_str());
        else
            DeleteFileW(partPath.c_str());
    } else {
        if(cf.is_open()) cf.close();
        DeleteFileW(partPath.c_str()); // incomplete download: no cache
    }
    shutdown(s, SD_SEND);
    closesocket(s);
    return true;
}

long long ParseRangeStart(const std::string& req){
    size_t p = req.find("Range:");
    if(p == std::string::npos) p = req.find("range:");
    if(p == std::string::npos) return 0;
    p = req.find("bytes=", p);
    if(p == std::string::npos) return 0;
    p += 6;
    long long v = 0;
    while(p < req.size() && isdigit((unsigned char)req[p])){ v = v*10 + (req[p]-'0'); ++p; }
    return v;
}

void HandleClient(SOCKET s){
    char buf[4096] = {0};
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if(n <= 0){ closesocket(s); return; }
    buf[n] = 0;
    std::string req(buf);

    // request line: GET /pid/tid.mp3 HTTP/1.1
    std::string path;
    if(req.rfind("GET ", 0) == 0){
        size_t sp = req.find(' ', 4);
        if(sp == std::string::npos) sp = req.find('\r', 4);
        if(sp != std::string::npos) path = req.substr(4, sp - 4);
    }

    long long pid = 0, tid = 0;
    {
        std::string ps = path.substr(1);
        size_t slash = ps.find('/');
        if(slash != std::string::npos){
            try{
                pid = std::stoll(ps.substr(0, slash));
                std::string ts = ps.substr(slash + 1);
                size_t dot = ts.find('.');
                if(dot != std::string::npos) ts = ts.substr(0, dot);
                tid = std::stoll(ts);
            }catch(...){}
        }
    }
    if(pid <= 0 || tid <= 0){
        SendAll(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        shutdown(s, SD_SEND); closesocket(s);
        return;
    }
    long long rangeStart = ParseRangeStart(req);

    // 1) 缓存命中 → 直接回本地文件
    if(TryServeCached(s, pid, tid, rangeStart)) return;

    // 2) resolve ladder: configured quality -> exhigh -> standard
    NcmConfig cfg; ConfigManager::Load(cfg);
    NcmClient client(cfg);
    std::wstring cfgLevel = cfg.quality.empty() ? L"exhigh" : cfg.quality;
    std::string cfgLevelA = WideToUtf8(cfgLevel);
    const char* ladder[3] = { cfgLevelA.c_str(), "exhigh", "standard" };
    std::string url, type, reason;
    bool resolved = false;
    for(int attempt = 0; attempt < 3 && !resolved; ++attempt){
        reason.clear();
        resolved = client.GetSongUrlLevel(tid, ladder[attempt], url, type, &reason) && !url.empty();
        if(!resolved){
            char b[192];
            sprintf_s(b, "resolve tid=%lld level=%s FAIL: %s", (long long)tid, ladder[attempt], reason.c_str());
            FsLog(b);
            url.clear();
        }
    }
    if(!resolved){
        SendAll(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        shutdown(s, SD_SEND); closesocket(s);
        return;
    }
    {
        char b[96];
        sprintf_s(b, "resolve tid=%lld OK type=%s", (long long)tid, type.c_str());
        FsLog(b);
    }

    // 3) 代理播放并落盘缓存
    std::wstring ext = Utf8ToWide(type.empty() ? "mp3" : type);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    ProxyAndCache(s, pid, tid, Utf8ToWide(url), ext, rangeStart);
}

void AcceptLoop(){
    while(g_run){
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if(c == INVALID_SOCKET) break;
        // one thread per connection: slow resolution/transfer never blocks other requests
        std::thread([c]{ HandleClient(c); }).detach();
    }
}

} // namespace

namespace LocalServer {

void RunCleanupNow(){
    CleanupOldCache();
}

bool Start(int preferredPort, int* boundPort){
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(g_listen == INVALID_SOCKET){ WSACleanup(); return false; }

    BOOL reuse = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only

    int port = preferredPort > 0 ? preferredPort : 47777;
    bool ok = false;
    for(int i = 0; i < 20; ++i){
        addr.sin_port = htons((u_short)(port + i));
        if(bind(g_listen, (sockaddr*)&addr, sizeof(addr)) == 0){
            ok = true; port = port + i; break;
        }
    }
    if(!ok || listen(g_listen, 8) != 0){
        closesocket(g_listen); g_listen = INVALID_SOCKET; WSACleanup();
        return false;
    }

    CleanupOldCache();

    g_run = true;
    g_acceptThread = std::thread(AcceptLoop);
    if(boundPort) *boundPort = port;
    return true;
}

void Stop(){
    g_run = false;
    if(g_listen != INVALID_SOCKET){
        closesocket(g_listen);      // unblock accept()
        g_listen = INVALID_SOCKET;
    }
    if(g_acceptThread.joinable()) g_acceptThread.join();
    WSACleanup();
}

} // namespace LocalServer
