#include "artwork_provider.h"
#include "config.h"
#include "ncm_client.h"
#include "utils.h"
#include <winhttp.h>
#include <string>
#include <fstream>

#pragma comment(lib, "winhttp.lib")

namespace {

std::wstring ArtCacheDir(){
    WCHAR tmp[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"aimp_ncm\\artwork";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

// 从本地代理条目 URI 提取歌曲ID: http://127.0.0.1:{port}/{pid}/{tid}.xxx
bool ParseLocalUri(const std::wstring& uri, long long& tid){
    size_t scheme = uri.find(L"://");
    if(scheme == std::string::npos) return false;
    size_t hostEnd = uri.find(L'/', scheme + 3);
    if(hostEnd == std::wstring::npos) return false;
    // 仅处理本机回环条目
    if(uri.compare(scheme + 3, 9, L"127.0.0.1") != 0) return false;
    size_t slash2 = uri.find(L'/', hostEnd + 1);
    if(slash2 == std::wstring::npos) return false;
    std::wstring ts = uri.substr(slash2 + 1);
    size_t dot = ts.find(L'.');
    if(dot != std::wstring::npos) ts = ts.substr(0, dot);
    try{ tid = std::stoll(ts); return tid > 0; }catch(...){ return false; }
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

} // namespace

NcmArtworkProvider::NcmArtworkProvider(IAIMPCore* core): core_(core){
    if(core_) core_->AddRef();
}

HRESULT WINAPI NcmArtworkProvider::QueryInterface(REFIID riid, void** ppv){
    if(!ppv) return E_POINTER;
    *ppv = nullptr;
    if(IsEqualIID(riid, IID_IUnknown))
        *ppv = static_cast<IUnknown*>(static_cast<IAIMPExtensionAlbumArtProvider*>(this));
    else if(IsEqualIID(riid, IID_IAIMPExtensionAlbumArtProvider))
        *ppv = static_cast<IAIMPExtensionAlbumArtProvider*>(this);
    else if(IsEqualIID(riid, IID_IAIMPExtensionAlbumArtProvider2))
        *ppv = static_cast<IAIMPExtensionAlbumArtProvider2*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
ULONG WINAPI NcmArtworkProvider::AddRef(){ return InterlockedIncrement(&ref_); }
ULONG WINAPI NcmArtworkProvider::Release(){
    ULONG c = InterlockedDecrement(&ref_);
    if(c == 0) delete this;
    return c;
}

HRESULT WINAPI NcmArtworkProvider::Get2(IAIMPFileInfo* FileInfo, IAIMPPropertyList* Options,
                                        IAIMPImageContainer** Image){
    if(!FileInfo || !Options || !Image) return E_INVALIDARG;
    *Image = nullptr;

    IAIMPString* uri = nullptr;
    if(FAILED(FileInfo->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, (void**)&uri)) || !uri)
        return E_FAIL;
    long long tid = 0;
    bool ours = ParseLocalUri(AimpStringToWString(uri), tid);
    uri->Release();
    if(!ours) return E_FAIL;

    // 1) 磁盘缓存命中
    std::wstring cachePath = ArtCacheDir() + L"\\" + std::to_wstring(tid) + L".img";
    HANDLE f = CreateFileW(cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if(f != INVALID_HANDLE_VALUE){
        LARGE_INTEGER li;
        GetFileSizeEx(f, &li);
        std::string bytes((size_t)li.QuadPart, 0);
        DWORD got = 0, total = 0;
        BOOL rd = TRUE;
        while(total < bytes.size() && (rd = ReadFile(f, bytes.data() + total, (DWORD)(bytes.size() - total), &got, nullptr)) && got)
            total += got;
        CloseHandle(f);
        bytes.resize(total);
        if(!bytes.empty()){
            if(SUCCEEDED(core_->CreateObject(IID_IAIMPImageContainer, (void**)Image)) && *Image){
                (*Image)->SetDataSize((LongWord)bytes.size());
                memcpy((*Image)->GetData(), bytes.data(), bytes.size());
                return S_OK;
            }
            return E_FAIL;
        }
    }

    // 2) 歌曲详情取封面链接
    NcmConfig cfg; ConfigManager::Load(cfg);
    NcmClient client(cfg);
    NcmSong song;
    if(!client.GetSongDetail(tid, song) || song.coverUrl.empty()) return E_FAIL;

    // 3) 下载封面字节(网易云图片服务支持 ?param=WxH 缩放, 平衡清晰度与体积)
    const std::wstring& picUrl = song.coverUrl;
    std::string bytes;
    if(!HttpDownloadBytes(picUrl + L"?param=500y500", bytes)){
        if(!HttpDownloadBytes(picUrl, bytes)) return E_FAIL;
    }

    // 4) 写缓存并返回
    {
        HANDLE w = CreateFileW(cachePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if(w != INVALID_HANDLE_VALUE){
            DWORD written = 0;
            WriteFile(w, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
            CloseHandle(w);
        }
    }
    if(SUCCEEDED(core_->CreateObject(IID_IAIMPImageContainer, (void**)Image)) && *Image){
        (*Image)->SetDataSize((LongWord)bytes.size());
        memcpy((*Image)->GetData(), bytes.data(), bytes.size());
        return S_OK;
    }
    return E_FAIL;
}
