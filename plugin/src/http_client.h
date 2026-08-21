#pragma once
#include <string>
#include <map>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

struct HttpResponse {
    int status = 0;
    std::string body;
    std::wstring headers;
    std::map<std::string,std::string> cookies;
};

class HttpClient {
public:
    static HttpResponse Get(const std::wstring& url, const std::wstring& headers = L"", const std::wstring& cookie = L"");
    static HttpResponse Post(const std::wstring& url, const std::string& data, const std::wstring& headers = L"", const std::wstring& cookie = L"");
    static std::wstring UrlEncode(const std::string& s);
    static std::string UrlEncodeW(const std::wstring& w);
private:
    static HttpResponse Request(const std::wstring& method, const std::wstring& url, const std::string& data, const std::wstring& headers, const std::wstring& cookie);
    static bool CrackUrl(const std::wstring& url, std::wstring& host, INTERNET_PORT& port, std::wstring& path, bool& https);
};
