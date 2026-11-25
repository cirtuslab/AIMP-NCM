// SPDX-License-Identifier: MIT
// winsock2 must be included before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include "security_utils.h"
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace NcmSec {

namespace {

bool IsRestrictedIPv4(uint32_t ip) {
    uint8_t a = (uint8_t)((ip >> 24) & 0xFF);
    uint8_t b = (uint8_t)((ip >> 16) & 0xFF);
    if (a == 0) return true;                                  // 0.0.0.0/8 (unspecified)
    if (a == 10) return true;                                 // 10.0.0.0/8 (RFC1918)
    if (a == 100 && (b & 0xC0) == 64) return true;            // 100.64.0.0/10 (CGNAT)
    if (a == 127) return true;                                // 127.0.0.0/8 (loopback)
    if (a == 169 && b == 254) return true;                    // 169.254.0.0/16 (link-local/metadata)
    if (a == 172 && (b >= 16 && b <= 31)) return true;        // 172.16.0.0/12 (RFC1918)
    if (a == 192 && b == 168) return true;                    // 192.168.0.0/16 (RFC1918)
    if (a >= 224) return true;                                // 224.0.0.0/4 multicast + 240.0.0.0/4 reserved/broadcast
    return false;
}

bool IsRestrictedIPv6(const unsigned char b[16]) {
    bool allZero = true;
    for (int i = 0; i < 16; ++i) if (b[i] != 0) { allZero = false; break; }
    if (allZero) return true;                                 // :: (unspecified)

    bool first15Zero = true;
    for (int i = 0; i < 15; ++i) if (b[i] != 0) { first15Zero = false; break; }
    if (first15Zero && b[15] == 1) return true;               // ::1 (loopback)

    if ((b[0] & 0xFE) == 0xFC) return true;                   // fc00::/7 (ULA)
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return true;   // fe80::/10 (link-local)
    if (b[0] == 0xFF) return true;                            // ff00::/8 (multicast)

    // IPv4-mapped ::ffff:a.b.c.d
    bool v4Mapped = true;
    for (int i = 0; i < 10; ++i) if (b[i] != 0) { v4Mapped = false; break; }
    if (v4Mapped && b[10] == 0xFF && b[11] == 0xFF) {
        uint32_t ip4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                       ((uint32_t)b[14] << 8)  | (uint32_t)b[15];
        return IsRestrictedIPv4(ip4);
    }

    // IPv4-compatible ::a.b.c.d
    bool v4Compat = true;
    for (int i = 0; i < 12; ++i) if (b[i] != 0) { v4Compat = false; break; }
    if (v4Compat) {
        uint32_t ip4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                       ((uint32_t)b[14] << 8)  | (uint32_t)b[15];
        return IsRestrictedIPv4(ip4);
    }
    return false;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 判断主机名是否可能是十进制/十六进制/八进制混淆 IPv4 (如 2130706433 / 0x7f000001 / 0177.0.0.1)
bool LooksLikeNumericHost(const std::wstring& h) {
    if (h.empty()) return true;
    if (h.find(L':') != std::wstring::npos) return true;
    size_t dot = h.find_last_of(L'.');
    std::wstring lastLabel = (dot == std::wstring::npos) ? h : h.substr(dot + 1);
    if (lastLabel.empty()) return true;
    bool allDigits = true;
    for (wchar_t c : lastLabel) {
        if (c < L'0' || c > L'9') { allDigits = false; break; }
    }
    if (allDigits) return true;
    if (lastLabel.size() > 2 && lastLabel[0] == L'0' && lastLabel[1] == L'x') {
        bool allHex = true;
        for (size_t i = 2; i < lastLabel.size(); ++i) {
            wchar_t c = lastLabel[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) { allHex = false; break; }
        }
        if (allHex) return true;
    }
    return false;
}

} // namespace

bool IsSafeExternalHost(const std::wstring& host) {
    if (host.empty()) return false;
    std::wstring h;
    h.reserve(host.size());
    for (wchar_t c : host) {
        if (c >= L'A' && c <= L'Z') h.push_back((wchar_t)(c - L'A' + L'a'));
        else h.push_back(c);
    }
    if (h.size() >= 2 && h.front() == L'[' && h.back() == L']')
        h = h.substr(1, h.size() - 2);
    while (!h.empty() && h.back() == L'.') h.pop_back();
    if (h.empty()) return false;

    if (h == L"localhost" || EndsWith(h, L".localhost") || EndsWith(h, L".local"))
        return false;

    IN_ADDR addr4{};
    if (InetPtonW(AF_INET, h.c_str(), &addr4) == 1) {
        return !IsRestrictedIPv4(ntohl(addr4.s_addr));
    }
    IN6_ADDR addr6{};
    if (InetPtonW(AF_INET6, h.c_str(), &addr6) == 1) {
        return !IsRestrictedIPv6(addr6.u.Byte);
    }

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW* res = nullptr;
    int gai = GetAddrInfoW(h.c_str(), nullptr, &hints, &res);
    bool wsaInit = false;
    if (gai == WSANOTINITIALISED) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
            wsaInit = true;
            gai = GetAddrInfoW(h.c_str(), nullptr, &hints, &res);
        }
    }
    if (gai == 0 && res) {
        bool safe = true;
        bool sawAddr = false;
        for (ADDRINFOW* p = res; p; p = p->ai_next) {
            if (p->ai_family == AF_INET) {
                sawAddr = true;
                auto* sin = (const sockaddr_in*)p->ai_addr;
                if (IsRestrictedIPv4(ntohl(sin->sin_addr.s_addr))) { safe = false; break; }
            } else if (p->ai_family == AF_INET6) {
                sawAddr = true;
                auto* sin6 = (const sockaddr_in6*)p->ai_addr;
                if (IsRestrictedIPv6(sin6->sin6_addr.u.Byte)) { safe = false; break; }
            }
        }
        FreeAddrInfoW(res);
        if (wsaInit) WSACleanup();
        return sawAddr && safe;
    }
    if (wsaInit) WSACleanup();

    if (LooksLikeNumericHost(h)) return false;
    return h.find(L'.') != std::wstring::npos;
}

bool IsSafeExternalUrl(const std::wstring& url) {
    if (url.empty()) return false;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    std::vector<WCHAR> host(512, 0);
    std::vector<WCHAR> path(4096, 0);
    uc.lpszHostName = host.data();
    uc.dwHostNameLength = (DWORD)host.size();
    uc.lpszUrlPath = path.data();
    uc.dwUrlPathLength = (DWORD)path.size();
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    std::wstring hostStr(uc.lpszHostName, uc.dwHostNameLength);
    return IsSafeExternalHost(hostStr);
}

} // namespace NcmSec
