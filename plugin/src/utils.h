#pragma once
#include <string>
#include <windows.h>
#include "../third_party/aimp_sdk/apiObjects.h"
#include "../third_party/aimp_sdk/apiCore.h"

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring AimpStringToWString(IAIMPString* s) {
    if (!s) return L"";
    WCHAR* buf = s->GetData();
    int len = s->GetLength();
    if(!buf) return L"";
    return std::wstring(buf, len);
}

// 归一化镜像地址: 去空白/尾部斜杠，无协议头时自动补 http://
// （如 "iwenwiki.com:3000" -> "http://iwenwiki.com:3000"，否则 WinHttpCrackUrl 失败）
inline std::wstring NormalizeApiUrl(const std::wstring& raw) {
    std::wstring u = raw;
    while(!u.empty() && (u.front()==L' '||u.front()==L'\t')) u.erase(u.begin());
    while(!u.empty() && (u.back()==L' '||u.back()==L'\t'||u.back()==L'/')) u.pop_back();
    if(u.empty()) return u;
    if(u.rfind(L"http://",0)!=0 && u.rfind(L"https://",0)!=0) u = L"http://" + u;
    return u;
}

// 归一化 Cookie: 去空白/引号；只粘贴了裸值(未带键名)时自动补 "MUSIC_U="
// （新版 MUSIC_U 可达数百字符，属正常）
inline std::wstring NormalizeCookie(const std::wstring& raw) {
    std::wstring c = raw;
    while(!c.empty() && (c.front()==L' '||c.front()==L'\t')) c.erase(c.begin());
    while(!c.empty() && (c.back()==L' '||c.back()==L'\t')) c.pop_back();
    if(c.size()>=2 && ((c.front()==L'"'&&c.back()==L'"')||(c.front()==L'\''&&c.back()==L'\'')))
        c = c.substr(1, c.size()-2);
    if(c.empty()) return c;
    if(c.find(L"MUSIC_U=")==std::wstring::npos) c = L"MUSIC_U=" + c;
    return c;
}
inline IAIMPString* WStringToAimpString(IAIMPCore* core, const std::wstring& w) {
    IAIMPString* s = nullptr;
    if (core->CreateObject(IID_IAIMPString, (void**)&s) == S_OK && s) {
        s->SetData((TChar*)w.c_str(), (int)w.size());
        return s;
    }
    return nullptr;
}
