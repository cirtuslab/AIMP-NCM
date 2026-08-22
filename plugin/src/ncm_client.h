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

struct QrLogin {
    std::string unikey;
    std::wstring qrUrl;
    std::string cookie;
};

class NcmClient {
public:
    NcmClient(const NcmConfig& cfg);
    void SetConfig(const NcmConfig& cfg){ cfg_=cfg; }
    bool QrCreate(QrLogin& out);
    // outChallengeUrl 非空时表示命中风控滑块/验证码，需在浏览器完成验证
    int QrCheck(const std::string& key, std::string& outCookie, std::wstring& outMsg, std::wstring* outChallengeUrl=nullptr, long long* outUid=nullptr);
    // 获取当前登录账号 uid (镜像 /user/account 或直连 weapi nuser/account/get)
    bool GetAccountId(long long& uid);
    bool GetUserPlaylists(long long uid, std::vector<NcmPlaylist>& out, int limit=100);
    bool GetPlaylistDetail(long long pid, std::vector<NcmSong>& outSongs, NcmPlaylist* info=nullptr);
    bool GetSongUrl(long long id, std::string& outUrl, std::string& outType);
    // 指定音质取链(供回退阶梯使用): level 如 standard/exhigh/lossless; reason 返回失败原因
    bool GetSongUrlLevel(long long id, const std::string& level, std::string& outUrl, std::string& outType, std::string* reason=nullptr);
    bool GetSongDetail(long long id, NcmSong& out);
    bool GetLyric(long long id, std::string& outLrc);
    bool IsLoggedIn();
    static long long ParseUidFromCookie(const std::wstring& cookie);
    std::wstring GetApiBase() const { return cfg_.apiUrl; }
private:
    NcmConfig cfg_;
    std::wstring deviceId_;  // 持久化的设备伪装 Cookie
    void EnsureDeviceCookie();
    std::string RequestMirror(const std::wstring& path, const std::string& jsonData, bool post=true);
    std::string RequestDirect(const std::wstring& uriPath, const std::string& jsonData, bool useEapi=false);
    std::string DoPost(const std::wstring& url, const std::string& body, const std::wstring& extraHeaders=L"");
};
