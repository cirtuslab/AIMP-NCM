#include "http_client.h"
#include "utils.h"
#include "error_notify.h"
#include <sstream>

bool HttpClient::CrackUrl(const std::wstring& url, std::wstring& host, INTERNET_PORT& port, std::wstring& path, bool& https) {
    URL_COMPONENTS uc={}; uc.dwStructSize=sizeof(uc);
    WCHAR h[256]={0}, p[2048]={0};
    uc.lpszHostName=h; uc.dwHostNameLength=256;
    uc.lpszUrlPath=p; uc.dwUrlPathLength=2048;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    host.assign(h, uc.dwHostNameLength);
    path.assign(p, uc.dwUrlPathLength);
    port = uc.nPort;
    https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}
HttpResponse HttpClient::Request(const std::wstring& method, const std::wstring& url, const std::string& data, const std::wstring& headers, const std::wstring& cookie) {
    HttpResponse resp;
    std::wstring host, path; INTERNET_PORT port; bool https;
    if (!CrackUrl(url, host, port, path, https)) return resp;
    HINTERNET hSession = WinHttpOpen(L"AIMP-NCM/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!hSession) return resp;
    DWORD timeout=15000; WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    // 补齐解析/发送超时（默认可能无限），确保测试连接最坏 15s 内返回，不会卡死
    WinHttpSetOption(hSession, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return resp; }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(), nullptr, nullptr, nullptr, flags);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return resp; }
    // 自动解压 gzip/deflate（网易云 weapi/eapi 响应默认 gzip 压缩，
    // 不解压则 body 是二进制乱码，JSON 解析必然失败）
#ifdef WINHTTP_OPTION_DECOMPRESSION
    {
        DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
    }
#endif
    std::wstring hdr = headers;
    // 声明不接受压缩编码，避免响应为 gzip 乱码（WinHTTP 默认会声明接受 gzip）
    // 注意: 需显式覆盖（REPLACE），且不能与自动解压同时用
    if (hdr.find(L"Accept-Encoding") == std::wstring::npos) {
        if (!hdr.empty()) hdr += L"\r\n";
        hdr += L"Accept-Encoding: identity";
    }
    if (!cookie.empty()) {
        if (!hdr.empty()) hdr += L"\r\n";
        hdr += L"Cookie: " + cookie;
    }
    if (!hdr.empty()) WinHttpAddRequestHeaders(hReq, hdr.c_str(), (ULONG)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    // ignore cert errors for simplicity
    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA|SECURITY_FLAG_IGNORE_CERT_CN_INVALID|SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    BOOL ok = FALSE;
    if (method==L"POST") {
        std::wstring ct = L"Content-Type: application/x-www-form-urlencoded";
        WinHttpAddRequestHeaders(hReq, ct.c_str(), (ULONG)-1, WINHTTP_ADDREQ_FLAG_ADD_IF_NEW);
        ok = WinHttpSendRequest(hReq, nullptr, 0, (LPVOID)(data.empty()?nullptr:data.c_str()), (DWORD)data.size(), (DWORD)data.size(), 0);
    } else {
        ok = WinHttpSendRequest(hReq, nullptr, 0, nullptr, 0, 0, 0);
    }
    if (!ok) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return resp; }
    if (!WinHttpReceiveResponse(hReq, nullptr)) { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return resp; }
    DWORD status=0, sz=sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    resp.status=(int)status;
    // 访问受限(403/429 等)统一弹窗警告(带冷却, 见 error_notify.cpp)
    NcmErrorNotifyAccess(resp.status);
    DWORD hdrLen=0; WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, nullptr, &hdrLen, nullptr);
    if (hdrLen) { std::wstring h(hdrLen/2,0); WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, h.data(), &hdrLen, nullptr); resp.headers=h; }
    // 解析 Set-Cookie（登录态 MUSIC_U 等从这里拿）
    {
        std::wstring hdr = resp.headers;
        size_t pos = 0;
        while((pos = hdr.find(L"Set-Cookie:", pos)) != std::wstring::npos){
            pos += 11;
            // 跳过可能的空格
            while(pos < hdr.size() && (hdr[pos]==L' '||hdr[pos]==L'\t')) pos++;
            size_t eol = hdr.find(L"\r\n", pos);
            if(eol==std::wstring::npos) eol = hdr.size();
            std::wstring line = hdr.substr(pos, eol-pos);
            // 取 "name=value" 部分（分号前）
            size_t semi = line.find(L';');
            if(semi!=std::wstring::npos) line = line.substr(0, semi);
            size_t eq = line.find(L'=');
            if(eq!=std::wstring::npos){
                std::string name = WideToUtf8(line.substr(0, eq));
                std::string val = WideToUtf8(line.substr(eq+1));
                // 去空白
                while(!name.empty() && (name.back()==' '||name.back()=='\t')) name.pop_back();
                while(!val.empty() && (val.front()==' '||val.front()=='\t')) val.erase(val.begin());
                if(!name.empty()) resp.cookies[name] = val;
            }
            pos = eol;
        }
    }
    // read body
    std::string body;
    DWORD avail=0;
    for(;;){
        if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
        if (avail==0) break;
        std::string buf(avail,0);
        DWORD read=0;
        if (!WinHttpReadData(hReq, buf.data(), avail, &read)) break;
        body.append(buf.data(), read);
        if (read==0) break; // 读不到数据即结束；chunked 下 read 可能小于 avail，继续循环
    }
    resp.body=body;
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return resp;
}
HttpResponse HttpClient::Get(const std::wstring& url, const std::wstring& headers, const std::wstring& cookie) {
    return Request(L"GET", url, "", headers, cookie);
}
HttpResponse HttpClient::Post(const std::wstring& url, const std::string& data, const std::wstring& headers, const std::wstring& cookie) {
    return Request(L"POST", url, data, headers, cookie);
}
std::wstring HttpClient::UrlEncode(const std::string& s) {
    std::string out; char hex[4];
    for(unsigned char c: s){ if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') out+=c; else { sprintf_s(hex,"%02X",c); out+='%'; out+=hex; } }
    return Utf8ToWide(out);
}
std::string HttpClient::UrlEncodeW(const std::wstring& w){ return WideToUtf8(UrlEncode(WideToUtf8(w))); }
