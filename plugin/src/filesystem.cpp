#include "filesystem.h"
#include "utils.h"
#include "config.h"
#include "meta_cache.h"
#include "../third_party/aimp_sdk/apiFileManager.h"
#include <shlwapi.h>
#include <fstream>
#pragma comment(lib, "shlwapi.lib")

// ---- 诊断日志: %TEMP%\aimp_ncm_fs.log ----
void FsLog(const char* what){
    WCHAR tmp[MAX_PATH]={0};
    if(!GetTempPathW(MAX_PATH, tmp)) return;
    std::wstring p = std::wstring(tmp) + L"aimp_ncm_fs.log";
    std::ofstream f(p.c_str(), std::ios::app);
    if(!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[40];
    sprintf_s(buf, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    f << buf << what << "\n";
}
void FsLogUri(const char* what, IAIMPString* s){
    std::wstring w = s ? AimpStringToWString(s) : L"(null)";
    FsLog((std::string(what) + " uri=" + WideToUtf8(w)).c_str());
}

NcmFileSystem::NcmFileSystem(IAIMPCore* core): core_(core) {
    if(core_) core_->AddRef();
}

HRESULT NcmFileSystem::QueryInterface(REFIID riid, void** ppv){
    if(!ppv) return E_POINTER;
    *ppv=nullptr;
    // FsLogGuid("QI", riid);  // 导入时查询量大, 需要时再打开
    if(IsEqualIID(riid, IID_IUnknown)) *ppv=static_cast<IUnknown*>(static_cast<IAIMPExtensionFileSystem*>(this));
    else if(IsEqualIID(riid, IID_IAIMPPropertyList)) *ppv=static_cast<IAIMPPropertyList*>(static_cast<IAIMPExtensionFileSystem*>(this));
    else if(IsEqualIID(riid, IID_IAIMPExtensionFileSystem)) *ppv=static_cast<IAIMPExtensionFileSystem*>(this);
    if(IsEqualIID(riid, IID_IAIMPFileSystemCommandDropSource)) *ppv=static_cast<IAIMPFileSystemCommandDropSource*>(this);
    else if(IsEqualIID(riid, IID_IAIMPFileSystemCommandStreaming)) *ppv=static_cast<IAIMPFileSystemCommandStreaming*>(this);
    else if(IsEqualIID(riid, IID_IAIMPFileSystemCommandFileInfo)) *ppv=static_cast<IAIMPFileSystemCommandFileInfo*>(this);
    else if(IsEqualIID(riid, IID_IAIMPExtensionFileInfoProvider)) *ppv=static_cast<IAIMPExtensionFileInfoProvider*>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
}
ULONG NcmFileSystem::AddRef(){ return InterlockedIncrement(&ref_); }
ULONG NcmFileSystem::Release(){ ULONG c=InterlockedDecrement(&ref_); if(c==0) delete this; return c; }

HRESULT NcmFileSystem::GetValueAsInt32(int PropertyID, int* Value){
    if(!Value) return E_POINTER;
    if(PropertyID==AIMP_FILESYSTEM_PROPID_READONLY){ *Value=1; return S_OK; }
    return E_NOTIMPL;
}
HRESULT NcmFileSystem::GetValueAsObject(int PropertyID, REFIID IID, void** Value){
    if(!Value) return E_POINTER;
    *Value=nullptr;
    if(PropertyID==AIMP_FILESYSTEM_PROPID_SCHEME && IsEqualIID(IID, IID_IAIMPString)){
        IAIMPString* s=nullptr;
        if(SUCCEEDED(core_->CreateObject(IID_IAIMPString, (void**)&s)) && s){
            s->SetData((TChar*)Scheme(), (int)wcslen(Scheme()));
            *Value=s;
            FsLog("Q: SCHEME -> ncm");
            return S_OK;
        }
        FsLog("Q: SCHEME CreateObject failed");
        return E_FAIL;
    }
    return E_NOTIMPL;
}
bool NcmFileSystem::Parse(const std::wstring& uri, long long& pid, long long& tid){
    // ncm://pid/tid.mp3  or ncm://pid/tid
    if(uri.rfind(L"ncm://",0)!=0) return false;
    std::wstring rest = uri.substr(6);
    size_t slash = rest.find(L'/');
    if(slash==std::wstring::npos) return false;
    try{
        pid = std::stoll(rest.substr(0,slash));
        std::wstring tidStr = rest.substr(slash+1);
        // strip .mp3
        size_t dot = tidStr.find(L'.');
        if(dot!=std::wstring::npos) tidStr = tidStr.substr(0,dot);
        tid = std::stoll(tidStr);
        return true;
    }catch(...){ return false; }
}
std::wstring NcmFileSystem::MakeUri(long long pid, long long tid){
    wchar_t buf[64];
    swprintf_s(buf, L"ncm://%lld/%lld.mp3", pid, tid);
    return buf;
}
bool NcmFileSystem::IsNcmUri(IAIMPString* s){
    if(!s) return false;
    std::wstring w = AimpStringToWString(s);
    long long pid,tid;
    return Parse(w,pid,tid);
}
HRESULT NcmFileSystem::CreateStream(IAIMPString* FileName, IAIMPStream** Stream){
    // DropSource 版本(2参数): AIMP 播放 ncm:// URI 的实际取流入口
    FsLog("CreateStream[DropSource] called");
    FsLogUri("  drop", FileName);
    return CreateStream(FileName, 0, 0, 0, Stream);
}
HRESULT NcmFileSystem::CreateStream(IAIMPString* FileName, const INT64 Offset, const INT64 Size, LongWord Flags, IAIMPStream** Stream){
    if(!Stream) return E_POINTER;
    *Stream=nullptr;
    FsLogUri("CreateStream[Streaming] called", FileName);
    if(!IsNcmUri(FileName)){ FsLog("  not ncm uri"); return E_FAIL; }
    std::wstring w = AimpStringToWString(FileName);
    long long pid=0,tid=0;
    if(!Parse(w,pid,tid)) return E_FAIL;

    NcmConfig cfg; ConfigManager::Load(cfg);
    NcmClient client(cfg);
    std::string url, type;
    if(!client.GetSongUrl(tid, url, type) || url.empty()){
        return E_FAIL;
    }
    std::wstring wurl = Utf8ToWide(url);
    // 委托给系统 HTTP FileSystem (服务对象必须用 QueryInterface 获取)
    IAIMPServiceFileStreaming* svc=nullptr;
    if(FAILED(core_->QueryInterface(IID_IAIMPServiceFileStreaming, (void**)&svc)) || !svc)
        return E_FAIL;
    IAIMPString* urlStr=nullptr;
    core_->CreateObject(IID_IAIMPString, (void**)&urlStr);
    if(urlStr) urlStr->SetData((TChar*)wurl.c_str(), (int)wurl.size());
    IAIMPVirtualFile* vf=nullptr;
    HRESULT hr = svc->CreateStreamForFileURI(urlStr, &vf, Stream);
    if(vf) vf->Release();
    if(urlStr) urlStr->Release();
    svc->Release();
    // Offset/Size 由 AIMP 处理，此处直接返回全流
    return hr;
}
HRESULT NcmFileSystem::GetFileAttrs(IAIMPString* FileName, TAIMPFileAttributes* Attrs){
    FsLogUri("GetFileAttrs called", FileName);
    if(!IsNcmUri(FileName)) return E_FAIL; // not handled -> fall through to default FS
    if(!Attrs) return E_POINTER;
    ZeroMemory(Attrs, sizeof(*Attrs));
    return S_OK;
}
HRESULT NcmFileSystem::GetFileSize(IAIMPString* FileName, INT64* Size){
    if(!IsNcmUri(FileName)) return E_FAIL;
    if(Size) *Size=0;
    return S_OK;
}
HRESULT NcmFileSystem::IsFileExists(IAIMPString* FileName){
    if(!IsNcmUri(FileName)) return E_FAIL;
    return S_OK;
}
HRESULT NcmFileSystem::GetFileInfo(IAIMPString* FileURI, IAIMPFileInfo* Info){
    if(!Info) return E_POINTER;
    FsLogUri("GetFileInfo called", FileURI);
    std::wstring w = AimpStringToWString(FileURI);
    long long pid=0,tid=0;
    if(!ParseStreamUri(w, pid, tid)) return E_FAIL;   // 非本插件条目 -> 交由其他 provider

    // 元数据与封面统一由 NcmMeta 提供(缓存优先, 未命中同步拉取并回写)
    NcmSong song;
    bool haveInfo = NcmMeta::Lookup(pid, tid, song);
    if(!haveInfo){
        NcmConfig cfg; ConfigManager::Load(cfg);
        NcmClient client(cfg);
        if(client.GetSongDetail(tid, song) && !song.title.empty()){
            NcmMeta::Upsert(pid, song);
            haveInfo = true;
        }
    }

    IAIMPString *s=nullptr;
    core_->CreateObject(IID_IAIMPString,(void**)&s);
    if(s){
        std::wstring t = (haveInfo && !song.title.empty()) ? song.title
                         : (L"NCM " + std::to_wstring(tid));
        s->SetData((TChar*)t.c_str(), (int)t.size());
        Info->SetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, s);
        s->Release();
    }
    if(haveInfo){
        if(!song.artist.empty()){
            IAIMPString* v = WStringToAimpString(core_, song.artist);
            if(v){ Info->SetValueAsObject(AIMP_FILEINFO_PROPID_ARTIST, v); v->Release(); }
        }
        if(!song.album.empty()){
            IAIMPString* v = WStringToAimpString(core_, song.album);
            if(v){ Info->SetValueAsObject(AIMP_FILEINFO_PROPID_ALBUM, v); v->Release(); }
        }
        if(song.durationMs > 0)
            Info->SetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, song.durationMs / 1000.0);
        // 封面作为 ALBUMART 属性附带(磁盘缓存/下载由 NcmMeta::GetCoverBytes 处理)
        std::string bytes;
        if(NcmMeta::GetCoverBytes(tid, song.coverUrl, bytes) && !bytes.empty()){
            IAIMPImageContainer* img = nullptr;
            if(SUCCEEDED(core_->CreateObject(IID_IAIMPImageContainer, (void**)&img)) && img){
                img->SetDataSize((LongWord)bytes.size());
                memcpy(img->GetData(), bytes.data(), bytes.size());
                Info->SetValueAsObject(AIMP_FILEINFO_PROPID_ALBUMART, img);
                img->Release();
            }
        }
    }

    // FileName 保持原 URI
    Info->SetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, FileURI);
    return S_OK;
}

bool NcmFileSystem::ParseStreamUri(const std::wstring& uri, long long& pid, long long& tid){
    if(uri.rfind(L"ncm://", 0) == 0){
        std::wstring rest = uri.substr(6);
        size_t slash = rest.find(L'/');
        if(slash == std::wstring::npos) return false;
        std::wstring tidStr = rest.substr(slash + 1);
        size_t dot = tidStr.find(L'.');
        if(dot != std::wstring::npos) tidStr = tidStr.substr(0, dot);
        try{ pid = std::stoll(rest.substr(0, slash)); tid = std::stoll(tidStr); return tid > 0; }
        catch(...){ return false; }
    }
    // http://127.0.0.1:{port}/{pid}/{tid}.{ext} (仅环回主机)
    size_t scheme = uri.find(L"://");
    if(scheme == std::wstring::npos) return false;
    size_t hostEnd = uri.find(L'/', scheme + 3);
    if(hostEnd == std::wstring::npos) return false;
    if(uri.compare(scheme + 3, 9, L"127.0.0.1") != 0) return false;
    size_t slash2 = uri.find(L'/', hostEnd + 1);
    if(slash2 == std::wstring::npos) return false;
    std::wstring tidStr = uri.substr(slash2 + 1);
    size_t dot = tidStr.find(L'.');
    if(dot != std::wstring::npos) tidStr = tidStr.substr(0, dot);
    try{
        pid = std::stoll(uri.substr(hostEnd + 1, slash2 - hostEnd - 1));
        tid = std::stoll(tidStr);
        return pid > 0 && tid > 0;
    }catch(...){ return false; }
}