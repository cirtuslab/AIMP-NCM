// SPDX-License-Identifier: MIT
#pragma once
#include <climits>
#include <cstring>
#include <string>

namespace NcmSec {

// H-1: 严格限制音频缓存文件扩展名白名单, 防止恶意镜像/响应通过 type 字段注入路径遍历
inline bool IsAllowedAudioExt(const std::wstring& ext) {
    return ext == L"mp3" || ext == L"flac" || ext == L"m4a" ||
           ext == L"wav" || ext == L"ape"  || ext == L"ogg" ||
           ext == L"aac";
}

inline std::wstring SanitizeAudioExt(const std::wstring& raw) {
    std::wstring ext;
    ext.reserve(raw.size());
    for (wchar_t c : raw) {
        if (c >= L'A' && c <= L'Z') ext.push_back((wchar_t)(c - L'A' + L'a'));
        else if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9')) ext.push_back(c);
        else return L"mp3"; // 含路径分隔符、点号或其他非法字符一律拒绝
    }
    return IsAllowedAudioExt(ext) ? ext : L"mp3";
}

// H-2: 安全解析 HTTP Range 起始偏移, 防止有符号整数溢出产生负值导致堆越界读
// 返回 true 表示合法(无 Range 头时 outStart=0); 返回 false 表示非法/溢出(调用方应回 416)
inline bool ParseRangeStartSafe(const std::string& req, long long& outStart) {
    outStart = 0;
    size_t hdrEnd = req.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) hdrEnd = req.size();

    size_t firstEol = req.find("\r\n");
    if (firstEol == std::string::npos || firstEol >= hdrEnd) return true;
    size_t lineStart = firstEol + 2;

    while (lineStart < hdrEnd) {
        size_t lineEnd = req.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > hdrEnd) lineEnd = hdrEnd;

        if (lineEnd - lineStart >= 6) {
            auto eqCi = [](char a, char b) {
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                return a == b;
            };
            const char* key = "range:";
            bool match = true;
            for (size_t i = 0; i < 6; ++i) {
                if (!eqCi(req[lineStart + i], key[i])) { match = false; break; }
            }
            if (match) {
                size_t p = lineStart + 6;
                while (p < lineEnd && (req[p] == ' ' || req[p] == '\t')) ++p;
                if (lineEnd - p < 6) return false;
                const char* unit = "bytes=";
                for (size_t i = 0; i < 6; ++i) {
                    if (!eqCi(req[p + i], unit[i])) return false;
                }
                p += 6;
                while (p < lineEnd && (req[p] == ' ' || req[p] == '\t')) ++p;
                // 不支持的后缀 Range (如 bytes=-500): 回退整段返回(200 OK)
                if (p < lineEnd && req[p] == '-') {
                    outStart = 0;
                    return true;
                }
                if (p >= lineEnd || req[p] < '0' || req[p] > '9') return false;

                long long v = 0;
                while (p < lineEnd && req[p] >= '0' && req[p] <= '9') {
                    int d = req[p] - '0';
                    if (v > (LLONG_MAX - d) / 10) return false; // 防止溢出回绕为负数
                    v = v * 10 + d;
                    ++p;
                }
                outStart = v;
                return true;
            }
        }
        if (lineEnd >= hdrEnd) break;
        lineStart = lineEnd + 2;
    }
    return true;
}

// H-4: 常量时间字符串比对(不提前因长度返回)
inline bool ConstantTimeEqual(const std::string& a, const std::string& b) {
    size_t maxLen = a.size() > b.size() ? a.size() : b.size();
    volatile unsigned char diff = (a.size() != b.size()) ? 1u : 0u;
    for (size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = i < a.size() ? (unsigned char)a[i] : 0u;
        unsigned char cb = i < b.size() ? (unsigned char)b[i] : 0u;
        diff = (unsigned char)(diff | (unsigned char)(ca ^ cb));
    }
    return diff == 0u;
}

// H-4: 日志脱敏(覆盖凭据键值与本地代理 URL 中的 /x/<token>/ 路径段)
inline std::string RedactSensitiveText(const std::string& s) {
    std::string out = s;
    const char* keys[] = {"MUSIC_U=", "cookie=", "X-NCM-Token:", "localToken", "mirrorToken"};
    for (const char* k : keys) {
        size_t pos = 0;
        size_t klen = std::strlen(k);
        while ((pos = out.find(k, pos)) != std::string::npos) {
            size_t v = pos + klen;
            while (v < out.size() && (out[v] == ' ' || out[v] == '\t')) ++v;
            size_t end = out.find_first_of(";\r\n \"&", v);
            if (end == std::string::npos) end = out.size();
            out.replace(v, end - v, "<redacted>");
            pos = v + 10;
        }
    }
    // 脱敏 /x/<token>/ 路径段 (例如 http://127.0.0.1:47777/x/<token>/pid/tid)
    {
        size_t pos = 0;
        while ((pos = out.find("/x/", pos)) != std::string::npos) {
            size_t tokStart = pos + 3;
            size_t tokEnd = out.find_first_of("/ \t\r\n;\"?&", tokStart);
            if (tokEnd == std::string::npos) tokEnd = out.size();
            if (tokEnd > tokStart) {
                out.replace(tokStart, tokEnd - tokStart, "<redacted>");
                pos = tokStart + 10;
            } else {
                pos = tokStart;
            }
        }
    }
    return out;
}

// H-5: 校验外部 CDN/封面主机名与 URL, 阻止 SSRF 访问回环、内网与链路本地地址
bool IsSafeExternalHost(const std::wstring& host);
bool IsSafeExternalUrl(const std::wstring& url);

} // namespace NcmSec
