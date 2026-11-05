// SPDX-License-Identifier: MIT
// winsock2 must be included before windows.h (pulled in by config.h)
#include <winsock2.h>
#include <ws2tcpip.h>
#include "local_server.h"
#include "config.h"
#include "ncm_client.h"
#include "ncm_crypto.h"
#include "utils.h"
#include "meta_cache.h"
#include "error_notify.h"
#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <cstring>
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
std::mutex g_srvMtx;                       // B3: 保护 Start/Stop 与 g_listen/g_acceptThread
std::mutex g_connMtx;                      // B2: 连接表
std::set<SOCKET> g_conns;
std::condition_variable g_connCv;
int g_boundPort = 0;
bool g_external = false;                   // 端口由另一个 AIMP 实例的本地服务占用并复用
bool g_wsaOwned = false;                   // 本实例是否持有 WSAStartup 引用(Stop 时配对释放)
const DWORD kConnStopWaitMs = 30000;       // B2: 卸载等待连接线程兜底上限
const int kMaxPortTries = 20;              // 端口被占用时最多向后尝试的个数
const DWORD kProbeTimeoutMs = 700;         // 复用探测: 建连+响应总超时

void ConnAdd(SOCKET c){
    std::lock_guard<std::mutex> lk(g_connMtx);
    g_conns.insert(c);
}
void ConnRemove(SOCKET c){
    std::lock_guard<std::mutex> lk(g_connMtx);
    g_conns.erase(c);
    g_connCv.notify_all();
}
void ConnShutdownAll(){
    std::lock_guard<std::mutex> lk(g_connMtx);
    for(SOCKET c : g_conns) shutdown(c, SD_SEND);
}
bool ConnWaitEmpty(DWORD ms){
    std::unique_lock<std::mutex> lk(g_connMtx);
    return g_connCv.wait_for(lk, std::chrono::milliseconds(ms),
                             []{ return g_conns.empty(); });
}

std::wstring CacheDir(){
    WCHAR tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"aimp_ncm\\cache";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void CleanupOldCache(){
    // 策略(L1): cfg.cacheDays<=0 表示永不自动删除(与 config.h 注释对齐, 此前 0 会被当 7 天); 白名单歌单(pid)的缓存跳过
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(cfg.cacheDays <= 0) return;

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

    const ULONGLONG maxAgeMs = (ULONGLONG)cfg.cacheDays * 24ull * 3600ull * 1000ull;
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
        // 修复: 之前用 GetTickCount64(开机毫秒) 与 FILETIME(1601 基准) 直接相减,
        // 无符号下溢导致每次启动都会删光缓存。改用系统时间(同为 1601 基准)比较。
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULONGLONG fileMs = (((ULONGLONG)wt.dwHighDateTime << 32) | wt.dwLowDateTime) / 10000;
        ULONGLONG nowMs  = (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) / 10000;
        if(nowMs > fileMs && nowMs - fileMs > maxAgeMs) DeleteFileW(full.c_str());
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

// 探测指定回环端口上是否已经有同一插件的可用本地服务。
// 仅当端口被另一个 AIMP 实例占用时调用: 建连/读响应都有短超时, 任何失败
// (拒绝/超时/非 200/缺少魔数)都视为不可复用, 调用方继续尝试下一个端口。
// 注意: 必须使用非阻塞 connect, 否则残留监听套接字(SYN 被丢弃)会让启动卡住。
bool ProbeExistingServer(int port, const std::string& token){
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool connected = false;
    u_long nonBlocking = 1;
    if(ioctlsocket(s, FIONBIO, &nonBlocking) == 0){
        int rc = connect(s, (sockaddr*)&addr, sizeof(addr));
        if(rc == 0){
            connected = true;
        } else if(WSAGetLastError() == WSAEWOULDBLOCK){
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{};
            tv.tv_sec = (long)(kProbeTimeoutMs / 1000);
            tv.tv_usec = (long)((kProbeTimeoutMs % 1000) * 1000);
            if(select(0, nullptr, &wf, nullptr, &tv) > 0){
                int err = 0, len = sizeof(err);
                if(getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0)
                    connected = true;
            }
        }
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
    }

    bool ok = false;
    if(connected){
        DWORD tmo = kProbeTimeoutMs;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
        std::string req = "GET /health/" + token +
                          " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if(SendAll(s, req)){
            char buf[256] = {0};
            int n = recv(s, buf, (int)sizeof(buf) - 1, 0);
            if(n > 0){
                std::string resp(buf, (size_t)n);
                ok = resp.find(" 200 ") != std::string::npos &&
                     resp.find("aimp_ncm_ok") != std::string::npos;
            }
        }
    }
    closesocket(s);
    return ok;
}

long long ParseRangeStart(const std::string& req);

// serve a local file directly (supports Range); returns whether handled.
// tag: 可选 ID3v2 前缀标签, 使虚拟流 = tag + 音频(客户端偏移需减去 tagSize 映射到文件)
// H2: 本函数与 ProxyAndCache 一律不关闭连接 socket, 统一由 HandleClient 的 RAII 收尾,
//     杜绝"失败路径关闭后调用方继续用/重关"的句柄复用误伤
bool ServeFile(SOCKET s, const std::wstring& path, const std::wstring& ext, long long reqStart, const std::string* tag){
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if(f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if(!GetFileSizeEx(f, &li)){ CloseHandle(f); return false; }
    long long tagSize = tag ? (long long)tag->size() : 0;
    long long size = li.QuadPart + tagSize;              // 虚拟流总长 = 标签 + 原音频
    if(reqStart >= size){
        // A7: 越界 Range → 416, 而不是当作未命中重新拉流
        std::string head = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                           "Content-Range: bytes */" + std::to_string(size) + "\r\n"
                           "Content-Length: 0\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
        SendAll(s, head);
        CloseHandle(f);
        return true;
    }
    bool partial = reqStart > 0;
    long long sendLen = size - reqStart;

    std::string head = partial
        ? "HTTP/1.1 206 Partial Content\r\n"
          "Content-Range: bytes " + std::to_string(reqStart) + "-" + std::to_string(size-1) + "/" + std::to_string(size) + "\r\n"
          "Content-Length: " + std::to_string(sendLen) + "\r\n"
        : "HTTP/1.1 200 OK\r\n"
          "Content-Length: " + std::to_string(sendLen) + "\r\n";
    head += std::string("Content-Type: ") + WideToUtf8(MimeFor(ext)) +
            "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
    if(!SendAll(s, head)){ CloseHandle(f); return true; }

    long long fileOff = reqStart >= tagSize ? reqStart - tagSize : 0;
    if(tag && reqStart < tagSize){
        // 请求落在标签区间: 先发标签的剩余部分, 再发文件开头
        long long tagLen = tagSize - reqStart;
        if(!SendAll(s, tag->data() + reqStart, (int)tagLen)){ CloseHandle(f); return true; }
        sendLen -= tagLen;
    }
    if(sendLen <= 0){ CloseHandle(f); return true; }

    LARGE_INTEGER pos; pos.QuadPart = fileOff;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    char buf[256*1024];   // per-connection thread: keep on stack, not static
    long long remaining = sendLen;
    while(remaining > 0){
        DWORD want = (DWORD)std::min<long long>(sizeof(buf), remaining), got = 0;
        if(!ReadFile(f, buf, want, &got, nullptr) || got == 0) break;
        if(!SendAll(s, buf, (int)got)) break;
        remaining -= got;
    }
    CloseHandle(f);
    return true;
}

// cache hit: 查找 {pid}_{tid}.* (.part 未完成文件除外)
bool TryServeCached(SOCKET s, long long pid, long long tid, long long rangeStart, const std::string* tag){
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
    return ServeFile(s, CacheDir() + L"\\" + name, ext, rangeStart, tag);
}

// ---------------- CDN streaming via raw WinHTTP handles (pipe to socket + disk cache) ----------------
struct HttpGet {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    long long total = -1;      // Content-Length, -1 when unknown
    std::wstring contentType;  // Content-Type 响应头(可能为空)
};

void HttpClose(HttpGet& g){
    if(g.req) WinHttpCloseHandle(g.req);
    if(g.con) WinHttpCloseHandle(g.con);
    if(g.ses) WinHttpCloseHandle(g.ses);
    g = {};
}

// 发起一次 GET 并返回响应; 自动跟随 302/301/303/307/308 重定向(最多 5 跳);
// TLS 默认严格校验; 仅配置的镜像 host 证书异常时放宽重试一次
bool HttpOpenGet(const std::wstring& url, HttpGet& g, int* outStatus = nullptr){
    if(outStatus) *outStatus = 0;
    std::wstring current = url;
    for(int hop = 0; hop < 6; ++hop){
        URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
        WCHAR host[256] = {0}, path[2048] = {0};
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 2048;
        if(!WinHttpCrackUrl(current.c_str(), (DWORD)current.size(), 0, &uc)){
            HttpClose(g);   // 确保不泄漏本跳已打开的句柄
            return false;
        }

        g.ses = WinHttpOpen(L"AIMP-NCM/1.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if(!g.ses) return false;
        DWORD t = 15000;
        WinHttpSetOption(g.ses, WINHTTP_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        WinHttpSetOption(g.ses, WINHTTP_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
        WinHttpSetOption(g.ses, WINHTTP_OPTION_RESOLVE_TIMEOUT, &t, sizeof(t));

        g.con = WinHttpConnect(g.ses, uc.lpszHostName, uc.nPort, 0);
        if(!g.con){ HttpClose(g); return false; }
        bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
        // M2: 默认严格 TLS; 仅用户配置的镜像 host 允许证书放宽一次。
        //     CDN/网易云域名禁止降级，避免被中间人替换音频或截获后续跳转。
        NcmConfig tlsCfg;
        ConfigManager::Load(tlsCfg);
        const bool allowRelax = https && HostAllowsTlsRelax(uc.lpszHostName, tlsCfg.apiUrl);
        const int maxRelax = allowRelax ? 1 : 0;
        bool reqOk = false;
        for(int relax = 0; relax <= maxRelax; ++relax){
            g.req = WinHttpOpenRequest(g.con, L"GET", uc.lpszUrlPath, nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
            if(!g.req) break;
            if(relax){
                FsLog("HttpOpenGet: mirror TLS cert check failed, retry once with relaxed flags");
                DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                 SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
                WinHttpSetOption(g.req, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
            }
            WinHttpAddRequestHeaders(g.req,
                L"User-Agent: AIMP-NCM/1.5\r\nReferer: https://music.163.com\r\n",
                (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

            if(WinHttpSendRequest(g.req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
               WinHttpReceiveResponse(g.req, nullptr)){
                reqOk = true;
                break;
            }

            DWORD err = GetLastError();
            WinHttpCloseHandle(g.req); g.req = nullptr;
            bool tlsErr = (err == ERROR_WINHTTP_SECURE_FAILURE ||
                           err == ERROR_WINHTTP_SECURE_CERT_CN_INVALID ||
                           err == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID ||
                           err == ERROR_WINHTTP_SECURE_INVALID_CA ||
                           err == ERROR_WINHTTP_SECURE_INVALID_CERT);
            if(!tlsErr || relax >= maxRelax) break;
        }
        if(!reqOk){ HttpClose(g); return false; }

        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(g.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status, &sz, nullptr);
        if(outStatus) *outStatus = (int)status;

        bool redirect = status == 301 || status == 302 || status == 303 ||
                        status == 307 || status == 308;
        if(!redirect){
            if(status != 200 && status != 206){
                NcmErrorNotifyAccess((int)status);   // 403/429 等访问受限弹窗警告
                HttpClose(g); return false;
            }
            WCHAR ct[128] = {0}; DWORD ctLen = sizeof(ct);
            if(WinHttpQueryHeaders(g.req, WINHTTP_QUERY_CONTENT_TYPE, nullptr, ct, &ctLen, nullptr))
                g.contentType.assign(ct, ctLen / sizeof(WCHAR));
            DWORD len = 0; sz = sizeof(len);
            if(WinHttpQueryHeaders(g.req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                   nullptr, &len, &sz, nullptr)) g.total = (long long)len;
            return true;
        }
        if(hop == 5){ HttpClose(g); return false; }   // 超过 5 跳: 放弃
        WCHAR loc[2048] = {0}; DWORD locLen = sizeof(loc);
        if(!WinHttpQueryHeaders(g.req, WINHTTP_QUERY_LOCATION, nullptr, loc, &locLen, nullptr)){
            HttpClose(g); return false;
        }
        // Location 可能是相对路径: 补全为绝对 URL 再进入下一跳
        std::wstring next(loc);
        if(!next.empty() && next.rfind(L"//", 0) == 0){
            next = (https ? L"https:" : L"http:") + next;   // 协议相对: //host/path
        } else if(!next.empty() && next[0] == L'/'){
            next = (https ? L"https://" : L"http://") + std::wstring(host) + next;
        } else if(next.find(L"://") == std::wstring::npos){
            size_t q = current.find(L'?');
            std::wstring base = q == std::wstring::npos ? current : current.substr(0, q);
            size_t slash = base.find_last_of(L'/');
            next = (slash == std::wstring::npos ? current : base.substr(0, slash + 1)) + next;
        }
        HttpClose(g);   // 释放旧句柄, 下一跳重开
        current = next;
    }
    return false;
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

// pipe CDN -> socket (only bytes >= contentSkip) while caching full content to disk.
// tag: 可选 ID3v2 前缀标签, 虚拟流 = tag + CDN 音频(客户端偏移需减去 tagSize 映射)
// client disconnect mid-way does NOT abort the download: next play hits the cache.
// H2: 返回 1=已处理(流已送达/416), 0=可重试失败(连接仍可用), -1=客户端已断开(上层应立即终止
//     重试且不弹"此曲不可用"); 连接 socket 一律不在此关闭, 由 HandleClient 的 RAII 统一收尾
int ProxyAndCache(SOCKET s, long long pid, long long tid, const std::wstring& url,
                  const std::wstring& ext, long long reqStart, const std::string* tag,
                  std::string* failReason = nullptr){
    HttpGet g;
    int cdnStatus = 0;
    if(!HttpOpenGet(url, g, &cdnStatus)){
        if(failReason){
            char b[64]; sprintf_s(b, "HTTP %d", cdnStatus);
            *failReason = std::string("CDN 连接失败 (") + b + ")";
        }
        return 0;
    }

    long long tagSize = tag ? (long long)tag->size() : 0;
    long long total = g.total;
    long long virtualTotal = total > 0 ? total + tagSize : -1;   // 虚拟流总长
    long long contentSkip = reqStart > tagSize ? reqStart - tagSize : 0;  // 音频内容跳过量

    // A7: Range 越界 → 416, 而不是继续拉流
    if(reqStart > 0 && virtualTotal > 0 && reqStart >= virtualTotal){
        std::string head416 = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                              "Content-Range: bytes */" + std::to_string(virtualTotal) + "\r\n"
                              "Content-Length: 0\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n";
        SendAll(s, head416);
        HttpClose(g);
        return 1;
    }

    // ---- A6: 内容校验(拒绝错误页/非音频响应), 失败不缓存并按"拉流失败"重试 ----
    std::string ct = WideToUtf8(g.contentType);
    std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
    bool badCtype = !ct.empty() &&
        (ct.find("text/html") != std::string::npos ||
         ct.find("text/plain") != std::string::npos ||
         ct.find("application/json") != std::string::npos ||
         ct.find("text/xml") != std::string::npos ||
         ct.find("application/xml") != std::string::npos);
    char probeBuf[16] = {0};
    int probeGot = HttpRead(g, probeBuf, (int)sizeof(probeBuf));
    if(probeGot < 0){
        if(failReason) *failReason = "读取 CDN 响应失败";
        HttpClose(g); return 0;
    }
    std::string prefix(probeBuf, probeGot);
    bool magicOk = true;
    if(!prefix.empty()){
        std::string e = WideToUtf8(ext);
        unsigned char b0 = (unsigned char)prefix[0];
        if(e == "flac")      magicOk = prefix.compare(0, 4, "fLaC") == 0;
        else if(e == "wav")  magicOk = prefix.compare(0, 4, "RIFF") == 0;
        else if(e == "m4a")  magicOk = prefix.size() >= 8 && prefix.compare(4, 4, "ftyp") == 0;
        // mp3/aac: MPEG 同步 0xFFE 或 ADTS(AAC) 同步 0xFFF, 或 ID3 前缀
        else                 magicOk = (b0 == 0xFF && ((unsigned char)prefix[1] & 0xE0) == 0xE0) ||
                                       prefix.compare(0, 3, "ID3") == 0;
    }
    if(badCtype || !magicOk){
        if(failReason) *failReason = "内容校验失败 (Content-Type: " + ct + ")";
        FsLog(("content check FAIL ct=" + ct + " ext=" + WideToUtf8(ext) +
               " magicOk=" + (magicOk ? "1" : "0")).c_str());
        HttpClose(g);
        return 0;       // 触发上层重试(最多3次), 全部失败后提示"此曲不可用"
    }

    std::string head;
    if(reqStart > 0 && virtualTotal > 0)
        head = "HTTP/1.1 206 Partial Content\r\n"
               "Content-Range: bytes " + std::to_string(reqStart) + "-" + std::to_string(virtualTotal-1) + "/" + std::to_string(virtualTotal) + "\r\n"
               "Content-Length: " + std::to_string(virtualTotal - reqStart) + "\r\n";
    else if(virtualTotal > 0)
        head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(virtualTotal) + "\r\n";
    else
        head = "HTTP/1.1 200 OK\r\n";
    head += "Accept-Ranges: bytes\r\nContent-Type: " + WideToUtf8(MimeFor(ext)) +
            "\r\nConnection: close\r\n\r\n";
    if(!SendAll(s, head)){
        // H2: 客户端尚未收到任何响应字节就断开 → 返回"连接已死",
        //     上层据此终止重试(不再空发 CDN 请求)且不误报"此曲不可用"
        if(failReason) *failReason = "客户端连接中断";
        HttpClose(g);
        return -1;
    }

    bool sendAlive = true;
    if(tag && reqStart < tagSize){
        // 请求落在标签区间: 先发标签的剩余部分
        if(!SendAll(s, tag->data() + reqStart, (int)(tagSize - reqStart))) sendAlive = false;
    }

    // 并发播放同一首歌时, 使用带线程ID+序号的临时文件, 避免互相截断写坏缓存
    static std::atomic<unsigned> g_partSeq{0};
    std::wstring partPath = CacheDir() + L"\\" + std::to_wstring(pid) + L"_" + std::to_wstring(tid) + L"." +
                            std::to_wstring(GetCurrentThreadId()) + L"." +
                            std::to_wstring(g_partSeq.fetch_add(1)) + L".part";
    std::ofstream cf(partPath.c_str(), std::ios::binary | std::ios::trunc);
    bool dlError = false;
    long long pos = 0;

    // 校验前缀也是音频内容: 先写缓存, 并按 contentSkip 决定是否转发给客户端
    if(!prefix.empty()){
        if(cf.is_open()){
            cf.write(prefix.data(), (std::streamsize)prefix.size());
            if(!cf.good()){ cf.close(); DeleteFileW(partPath.c_str()); } // disk error: drop cache
        }
        if(sendAlive){
            if(pos < contentSkip){
                long long skip = std::min<long long>((long long)prefix.size(), contentSkip - pos);
                if(skip < (long long)prefix.size())
                    if(!SendAll(s, prefix.data() + skip, (int)(prefix.size() - skip))) sendAlive = false;
            } else {
                if(!SendAll(s, prefix.data(), (int)prefix.size())) sendAlive = false;
            }
        }
        pos += (long long)prefix.size();
    }

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
            if(pos < contentSkip){
                long long skip = std::min<long long>(r, contentSkip - pos);
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
        // D4: 原子替换, 避免并发读者看到缺失/半截文件
        if(MoveFileExW(partPath.c_str(), finalPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            FsLog(("cached tid=" + std::to_string(tid)).c_str());
        else
            DeleteFileW(partPath.c_str());
    } else {
        if(cf.is_open()) cf.close();
        DeleteFileW(partPath.c_str()); // incomplete download: no cache
    }
    return 1;   // H2: 连接由 HandleClient 的 RAII 统一关闭
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

// H2: 连接 socket 的 RAII 所有权 —— HandleClient 的所有退出路径(含异常展开)统一
// shutdown + closesocket, ServeFile/ProxyAndCache 等被调函数不再触碰连接生命周期
struct ConnCloser {
    SOCKET s;
    explicit ConnCloser(SOCKET c): s(c) {}
    ~ConnCloser(){
        if(s != INVALID_SOCKET){
            shutdown(s, SD_SEND);
            closesocket(s);
        }
    }
};

// ---- 本地代理鉴权 ----
// 播放列表条目形如 http://127.0.0.1:{port}/x/{token}/{pid}/{tid}.mp3,
// token 段用于防止本机其他进程(恶意网页/文档)直接借用代理拉流。
// 校验失败统一返回 403, 不泄露任何内部信息。
// 空 token 一律拒绝(不再放行): Start() 首次启动会自动生成; 老播放列表需重新「应用」同步。
bool AuthTokenOk(const std::string& path){
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(cfg.localToken.empty()) return false;
    // 期望路径: /x/{token}/{pid}/{tid}
    const std::string kPrefix = "/x/";
    if(path.rfind(kPrefix, 0) != 0) return false;
    std::string rest = path.substr(kPrefix.size());
    size_t slash = rest.find('/');
    if(slash == std::string::npos) return false;
    std::string tok = rest.substr(0, slash);
    return SecureEquals(tok, WideToUtf8(cfg.localToken));
}
// 去掉路径中的 token 段, 得到 /{pid}/{tid} 形式
bool StripTokenPath(const std::string& path, std::string& out){
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(cfg.localToken.empty()) return false;
    const std::string kPrefix = "/x/";
    if(path.rfind(kPrefix, 0) != 0) return false;
    std::string rest = path.substr(kPrefix.size());
    size_t slash = rest.find('/');
    if(slash == std::string::npos) return false;
    out = "/" + rest.substr(slash + 1);
    return true;
}

void HandleClientInner(SOCKET s){
    // H1: 循环 recv 直到请求头结束(\r\n\r\n)或缓冲区满, 防止 TCP 分段(如只收到 "GE")
    //     被当成完整请求解析; 加收超时, 防慢速连接长期占用连接线程
    DWORD rcvTimeout = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));
    char buf[4096] = {0};
    int n = 0;
    while(n < (int)sizeof(buf) - 1){
        int r = recv(s, buf + n, (int)sizeof(buf) - 1 - n, 0);
        if(r <= 0) return;
        n += r;
        buf[n] = 0;
        if(strstr(buf, "\r\n\r\n")) break;
    }
    buf[n] = 0;
    std::string req(buf);

    // request line: GET /pid/tid.mp3 HTTP/1.1
    // H1: 仅接受 GET; 非 GET/空路径显式拒绝, 不再让空 path 进入 substr 抛 out_of_range
    if(req.rfind("GET ", 0) != 0){
        SendAll(s, "HTTP/1.1 405 Method Not Allowed\r\n"
                   "Content-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    std::string path;
    {
        size_t sp = req.find(' ', 4);
        if(sp == std::string::npos) sp = req.find('\r', 4);
        if(sp != std::string::npos) path = req.substr(4, sp - 4);
    }
    if(path.empty() || path[0] != '/'){
        SendAll(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }

    // 存活探测(仅供本机另一个 AIMP 实例决定是否复用本服务):
    //   GET /health/{localToken}
    // 不访问网络/缓存, 也不返回任何配置内容; token 不匹配一律 403。
    {
        const std::string kHealthPrefix = "/health/";
        if(path.rfind(kHealthPrefix, 0) == 0){
            NcmConfig cfg; ConfigManager::Load(cfg);
            std::string tok = path.substr(kHealthPrefix.size());
            size_t q = tok.find('?');
            if(q != std::string::npos) tok.resize(q);
            if(!cfg.localToken.empty() && SecureEquals(tok, WideToUtf8(cfg.localToken))){
                const std::string body = "aimp_ncm_ok";
                SendAll(s, "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n"
                           "Connection: close\r\n\r\n" + body);
            } else {
                SendAll(s, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
            return;
        }
    }

    // 鉴权: 未设置 token 时放行(兼容旧版播放列表条目); 否则路径必须带正确 token 段
    if(!AuthTokenOk(path)){
        SendAll(s, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    if(!StripTokenPath(path, path)){
        SendAll(s, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
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
        return;
    }
    long long rangeStart = ParseRangeStart(req);

    // 构建 ID3v2 流标签(标题/歌手/专辑/时长/封面) — 由 NcmMeta 从元数据缓存生成,
    // 使 AIMP 从音频流中解析出歌曲信息(网络流不经 FileInfoProvider 回调)
    std::string tagBytes = NcmMeta::BuildStreamTag(pid, tid);
    const std::string* tag = tagBytes.empty() ? nullptr : &tagBytes;

    // 1) 缓存命中 → 直接回本地文件
    if(TryServeCached(s, pid, tid, rangeStart, tag)) return;

    // 2) 取链: 按配置音质从高到低回退, 取第一个可用的"最高音质"
    //    (配置音质不存在时自动降级; 这不是"拉流失败降级")
    NcmConfig cfg; ConfigManager::Load(cfg);
    NcmClient client(cfg);
    std::wstring cfgLevel = cfg.quality.empty() ? L"exhigh" : cfg.quality;
    const char* kLevels[] = {"sky","jyeffect","jymaster","hires","lossless","exhigh","higher","standard"};
    int startIdx = 5;   // exhigh 默认
    std::string cfgA = WideToUtf8(cfgLevel);
    for(int i=0;i<8;i++){ if(cfgA == kLevels[i]){ startIdx = i; break; } }
    std::string url, type, reason, usedLevel;
    std::wstring resolveDetails;   // 取链阶段失败记录
    bool transportFatal = false;   // 网络/接口不可达(与"该曲无版权"区分, 避免误导用户)
    for(int i=startIdx; i<8; ++i){
        reason.clear();
        bool fatal = false;
        if(client.GetSongUrlLevel(tid, kLevels[i], url, type, &reason, &fatal) && !url.empty()){
            usedLevel = kLevels[i];
            break;
        }
        char b[192];
        sprintf_s(b, "resolve tid=%lld level=%s FAIL: %s", (long long)tid, kLevels[i], reason.c_str());
        FsLog(b);
        resolveDetails += L"  " + Utf8ToWide(kLevels[i]) + L"：" + Utf8ToWide(reason) + L"\n";
        url.clear();
        // 请求层失败(网络不可达/空响应/非 JSON): 换下一个音质只会重复同一个失败请求,
        // 直接结束阶梯。此前一首歌会被放大成 5 次请求(实测 2550/2685 均属此类)。
        if(fatal){
            transportFatal = true;
            FsLog(("resolve tid=" + std::to_string(tid) +
                   " abort level-ladder on transport failure").c_str());
            resolveDetails += L"  （网络/接口不可达，已停止音质降级）\n";
            break;
        }
    }
    if(url.empty()){
        NcmSong song;
        std::wstring title;
        if(NcmMeta::LookupByTid(tid, song) && !song.title.empty()) title = song.title;
        else title = L"tid " + std::to_wstring(tid);
        if(transportFatal){
            // 网络整体不可用: 逐曲弹"此曲不可用"既是误报又会刷屏, 只记日志。
            // 播放失败本身由 AIMP 呈现, 网络恢复后下次播放自动恢复。
            FsLog(("resolve tid=" + std::to_string(tid) +
                   " skipped track-unavailable popup (transport failure)").c_str());
        } else {
            // 全部音质都取不到链: 明确告知用户此首不可用(每首 30s 冷却, 防连续失败刷屏)
            NcmErrorNotifyTrackUnavailable(tid, title, resolveDetails);
        }
        SendAll(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    std::wstring ext = Utf8ToWide(type.empty() ? "mp3" : type);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);

    // 3) 拉流: 下载失败不做降级, 同一链接后台重试最多 3 次,
    //    3 次均失败则提示用户"此首不可用"并返回 404
    //    (H2: 客户端先断开的情况除外 —— 终止重试且不弹"此曲不可用")
    const int kMaxStreamAttempts = 3;
    bool streamed = false, clientGone = false;
    std::wstring streamDetails;   // 逐次拉流失败记录
    for(int attempt = 1; attempt <= kMaxStreamAttempts && !streamed && !clientGone; ++attempt){
        std::string failReason;
        int r = ProxyAndCache(s, pid, tid, Utf8ToWide(url), ext, rangeStart, tag, &failReason);
        if(r > 0){
            char b[96];
            sprintf_s(b, "stream tid=%lld OK type=%s", (long long)tid, type.c_str());
            FsLog(b);
            streamed = true;
            break;
        }
        if(r < 0){ clientGone = true; break; }
        char b[192];
        sprintf_s(b, "stream tid=%lld level=%s attempt=%d FAIL", (long long)tid, usedLevel.c_str(), attempt);
        FsLog(b);
        streamDetails += L"  第 " + std::to_wstring(attempt) + L" 次：" + Utf8ToWide(failReason) + L"\n";
    }
    if(!streamed && !clientGone){
        NcmSong song;
        std::wstring title;
        if(NcmMeta::LookupByTid(tid, song) && !song.title.empty()) title = song.title;
        else title = L"tid " + std::to_wstring(tid);
        NcmErrorNotifyTrackUnavailable(tid, title, streamDetails);
        SendAll(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
}

void HandleClient(SOCKET s){
    ConnCloser closer(s);   // H2: RAII 统一收尾连接
    try{
        // H1: 任何未预料的解析/内存异常都不允许穿出到 std::thread
        //     (否则 std::terminate 直接终止整个 AIMP 进程)
        HandleClientInner(s);
    }catch(...){
        try{ FsLog("HandleClient: unexpected exception, connection dropped"); }catch(...){}
    }
}

void AcceptLoop(){
    while(g_run){
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if(c == INVALID_SOCKET) break;
        // B2: 连接登记到表, 线程结束后移除; Stop 时 shutdown + 等待
        ConnAdd(c);
        try{
            std::thread([c]{
                HandleClient(c);
                ConnRemove(c);
            }).detach();
        }catch(...){
            // H1: 线程资源耗尽时不能泄漏 socket(否则 Stop 的等待永远等不到空表)
            ConnRemove(c);
            shutdown(c, SD_SEND);
            closesocket(c);
        }
    }
}

} // namespace

namespace LocalServer {

void RunCleanupNow(){
    CleanupOldCache();
}

bool IsExternal(){
    std::lock_guard<std::mutex> lk(g_srvMtx);
    return g_external;
}

bool Start(int preferredPort, int* boundPort){
    std::lock_guard<std::mutex> lk(g_srvMtx);   // B3: Start/Stop 互斥
    if(g_run || g_external){ if(boundPort) *boundPort = g_boundPort; return true; }
    // 本地代理 token: 首次启动时自动生成并持久化(防止本机其他进程借用代理)。
    // CSPRNG 失败时拒绝启动, 绝不在空 token 下监听。
    std::string token;
    {
        NcmConfig cfg; ConfigManager::Load(cfg);
        if(cfg.localToken.empty()){
            for(int i = 0; i < 4 && cfg.localToken.empty(); ++i){
                std::string rnd = NcmCrypto::RandomString(24);
                if(rnd.size() >= 24) cfg.localToken = Utf8ToWide(rnd);
            }
            if(cfg.localToken.empty()){
                FsLog("LocalServer start FAILED: cannot generate localToken (CSPRNG)");
                return false;
            }
            ConfigManager::Save(cfg);
        }
        token = WideToUtf8(cfg.localToken);
    }
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;

    const int basePort = preferredPort > 0 ? preferredPort : 47777;
    for(int i = 0; i < kMaxPortTries; ++i){
        const int port = basePort + i;
        if(port <= 0 || port > 65535) break;

        // 每个候选端口独立建 socket: 失败后不复用, 避免残留状态影响下一次 bind。
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if(s == INVALID_SOCKET) continue;

        // 关键修复: Windows 下 SO_REUSEADDR 允许多个进程 bind 同一端口, 且连接
        // 会被丢进"无人 accept"的监听队列(表现为 AIMP 报 Can't open the file,
        // 插件日志里连 GetFileAttrs 都没有)。SO_EXCLUSIVEADDRUSE 保证同一端口
        // 只有一个监听者, 被占用时 bind 返回 WSAEADDRINUSE, 从而真正触发换端口。
        // 已验证: 该选项仍允许重启后在 TIME_WAIT/FIN_WAIT_2 残留连接上重新监听。
        BOOL exclusive = TRUE;
        if(setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                      (const char*)&exclusive, sizeof(exclusive)) != 0){
            closesocket(s);
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only
        addr.sin_port = htons((u_short)port);

        if(bind(s, (sockaddr*)&addr, sizeof(addr)) != 0){
            const int err = WSAGetLastError();
            closesocket(s);
            // 端口被另一个 AIMP 实例占用时, 先确认对方是"可用的同插件服务":
            // 可用则复用它(多开 AIMP 共用一份服务器, 播放列表端口保持不变);
            // 残留/无响应的监听者(僵尸进程)探测必然失败, 继续换下一个端口。
            if(err == WSAEADDRINUSE && ProbeExistingServer(port, token)){
                g_external = true;
                g_boundPort = port;
                g_wsaOwned = false;
                WSACleanup();
                char b[128];
                sprintf_s(b, "LocalServer: reusing existing instance on 127.0.0.1:%d", port);
                FsLog(b);
                if(boundPort) *boundPort = port;
                return true;
            }
            char b[112];
            sprintf_s(b, "LocalServer: port %d unavailable (wsa=%d), trying next", port, err);
            FsLog(b);
            continue;
        }

        if(listen(s, 8) != 0){
            closesocket(s);
            FsLog("LocalServer: listen FAILED, trying next port");
            continue;
        }

        g_listen = s;
        g_boundPort = port;
        g_run = true;
        g_external = false;
        g_wsaOwned = true;
        try{
            g_acceptThread = std::thread(AcceptLoop);
        }catch(...){
            // 线程创建失败: 回滚监听状态, 否则后续 Start 会误以为服务已在运行
            g_run = false;
            closesocket(g_listen);
            g_listen = INVALID_SOCKET;
            g_boundPort = 0;
            g_wsaOwned = false;
            WSACleanup();
            FsLog("LocalServer start FAILED: cannot create accept thread");
            return false;
        }
        // 放到线程启动之后: 即使清理缓存抛异常, 服务本身也已处于可用的自洽状态
        CleanupOldCache();
        if(port != basePort){
            char b[176];
            sprintf_s(b, "LocalServer: preferred port %d busy, using %d "
                         "(re-sync playlist if URLs still point to %d)",
                      basePort, port, basePort);
            FsLog(b);
        }
        if(boundPort) *boundPort = port;
        return true;
    }

    WSACleanup();
    FsLog("LocalServer start FAILED: no free loopback port");
    return false;
}

void Stop(){
    std::lock_guard<std::mutex> lk(g_srvMtx);   // B3: Start/Stop 互斥
    // 复用他人服务时本实例没有监听套接字, 直接归还状态(不能 WSACleanup 别人的引用)
    if(g_external){
        g_external = false;
        g_boundPort = 0;
        return;
    }
    g_run = false;
    if(g_listen != INVALID_SOCKET){
        closesocket(g_listen);      // unblock accept()
        g_listen = INVALID_SOCKET;
    }
    if(g_acceptThread.joinable()) g_acceptThread.join();
    ConnShutdownAll();                          // B2: 通知连接线程退出
    ConnWaitEmpty(kConnStopWaitMs);             // B2: 等待(兜底 30s)后再清理 Winsock
    if(g_wsaOwned){ WSACleanup(); g_wsaOwned = false; }
}

} // namespace LocalServer
