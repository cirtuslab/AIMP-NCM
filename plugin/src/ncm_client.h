// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>
#include <map>
#include "config.h"

struct NcmSong {
    long long id=0;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int durationMs=0;
    std::wstring coverUrl;
};

struct NcmPlaylist {
    long long id=0;
    std::wstring name;
    int trackCount=0;
    std::wstring coverUrl;
    std::wstring creator;
};

class NcmClient {
public:
    NcmClient(const NcmConfig& cfg);
    void SetConfig(const NcmConfig& cfg){ cfg_=cfg; }
    // 获取当前登录账号 uid (镜像 /user/account 或直连 weapi nuser/account/get)
    bool GetAccountId(long long& uid);
    bool GetUserPlaylists(long long uid, std::vector<NcmPlaylist>& out, int limit=100);
    bool GetPlaylistDetail(long long pid, std::vector<NcmSong>& outSongs, NcmPlaylist* info=nullptr);
    bool GetSongUrl(long long id, std::string& outUrl, std::string& outType);
    // 指定音质取链(供回退阶梯使用): level 如 standard/exhigh/lossless; reason 返回失败原因
    // fatal=true 表示请求层失败(空响应/非 JSON), 换音质无意义, 调用方应停止降档阶梯
    bool GetSongUrlLevel(long long id, const std::string& level, std::string& outUrl, std::string& outType, std::string* reason=nullptr, bool* fatal=nullptr);
    bool GetSongDetail(long long id, NcmSong& out);
    // 取歌词; noLyric 输出 true 表示服务端明确答复"无歌词"(区别于网络失败)
    bool GetLyric(long long id, std::string& outLrc, bool* noLyric=nullptr);
    bool IsLoggedIn();
    static long long ParseUidFromCookie(const std::wstring& cookie);
    // 设备指纹: 持久化于 config.deviceCookie, 仅登录/换 Cookie 时重新生成
    static std::wstring BuildDeviceCookie();
    static void RegenerateDeviceCookie();
    std::wstring GetApiBase() const { return cfg_.apiUrl; }
private:
    NcmConfig cfg_;
    std::wstring deviceId_;  // 持久化的设备伪装 Cookie
    void EnsureDeviceCookie();
    std::string RequestMirror(const std::wstring& path, const std::string& jsonData, bool post=true, int* outStatus=nullptr);
    std::string RequestDirect(const std::wstring& uriPath, const std::string& jsonData, bool useEapi=false);
};
