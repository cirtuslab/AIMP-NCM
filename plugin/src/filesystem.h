#pragma once
#include <string>
#include <windows.h>
#include "../third_party/aimp_sdk/apiFileManager.h"
#include "../third_party/aimp_sdk/apiObjects.h"
#include "../third_party/aimp_sdk/apiCore.h"

// 完整 ncm:// 虚拟文件系统，支持懒加载播放
// 注意: 播放取流走 IAIMPFileSystemCommandDropSource::CreateStream(2参数版),
// 参考 AIMPYouTube —— 只实现 CommandStreaming(5参数版)会导致 Unsupported protocol
class NcmFileSystem : public IAIMPExtensionFileSystem,
                     public IAIMPFileSystemCommandDropSource,
                     public IAIMPFileSystemCommandStreaming,
                     public IAIMPFileSystemCommandFileInfo,
                     public IAIMPExtensionFileInfoProvider {
public:
    explicit NcmFileSystem(IAIMPCore* core);
    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    // IAIMPPropertyList
    HRESULT WINAPI GetValueAsFloat(int PropertyID, double* Value) override { return E_NOTIMPL; }
    HRESULT WINAPI SetValueAsFloat(int PropertyID, double Value) override { return E_NOTIMPL; }
    HRESULT WINAPI GetValueAsInt32(int PropertyID, int* Value) override;
    HRESULT WINAPI SetValueAsInt32(int PropertyID, int Value) override { return E_NOTIMPL; }
    HRESULT WINAPI GetValueAsInt64(int PropertyID, INT64* Value) override { return E_NOTIMPL; }
    HRESULT WINAPI SetValueAsInt64(int PropertyID, INT64 Value) override { return E_NOTIMPL; }
    HRESULT WINAPI GetValueAsObject(int PropertyID, REFIID IID, void** Value) override;
    HRESULT WINAPI SetValueAsObject(int PropertyID, IUnknown* Value) override { return E_NOTIMPL; }
    void WINAPI BeginUpdate() override {}
    void WINAPI EndUpdate() override {}
    HRESULT WINAPI Reset() override { return S_OK; }
    // IAIMPFileSystemCommandStreaming (5参数)
    HRESULT WINAPI CreateStream(IAIMPString* FileName, const INT64 Offset, const INT64 Size, LongWord Flags, IAIMPStream** Stream) override;
    // IAIMPFileSystemCommandDropSource (2参数) — 播放 ncm:// 的实际取流入口
    HRESULT WINAPI CreateStream(IAIMPString* FileName, IAIMPStream** Stream) override;
    // IAIMPFileSystemCommandFileInfo
    HRESULT WINAPI GetFileAttrs(IAIMPString* FileName, TAIMPFileAttributes* Attrs) override;
    HRESULT WINAPI GetFileSize(IAIMPString* FileName, INT64* Size) override;
    HRESULT WINAPI IsFileExists(IAIMPString* FileName) override;
    // IAIMPExtensionFileInfoProvider
    HRESULT WINAPI GetFileInfo(IAIMPString* FileURI, IAIMPFileInfo* Info) override;

    // SCHEME 返回裸协议名(参考 AIMPYouTube: "youtube"), AIMP 自行匹配 ncm:// URI
    static const wchar_t* Scheme() { return L"ncm"; }
    static bool Parse(const std::wstring& uri, long long& pid, long long& tid);
    static std::wstring MakeUri(long long pid, long long tid);

    // Helper for m3u generation (fallback when FileSystem not registered)
    static std::wstring MakeFileName(long long pid, long long trackId, const std::wstring& title, const std::wstring& artist) {
        wchar_t buf[1024];
        swprintf_s(buf, L"ncm://%lld/%lld.mp3", pid, trackId);
        return buf;
    }

private:
    IAIMPCore* core_ = nullptr;
    volatile LONG ref_ = 1;
    bool IsNcmUri(IAIMPString* s);
};
