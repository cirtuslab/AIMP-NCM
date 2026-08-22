#pragma once
#include <windows.h>
#include "../third_party/aimp_sdk/apiAlbumArt.h"
#include "../third_party/aimp_sdk/apiCore.h"

// 网易云歌曲封面提供者:
// 匹配本地代理条目(http://127.0.0.1:{port}/{pid}/{tid}.xxx),
// 通过歌曲详情接口取 al.picUrl 下载封面, 带磁盘缓存。
class NcmArtworkProvider : public IAIMPExtensionAlbumArtProvider2 {
public:
    explicit NcmArtworkProvider(IAIMPCore* core);
    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    // IAIMPExtensionAlbumArtProvider
    HRESULT WINAPI Get(IAIMPString* FileURI, IAIMPString* Artist,
                       IAIMPString* Album, IAIMPPropertyList* Options,
                       IAIMPImageContainer** Image) override { return E_NOTIMPL; }
    LongWord WINAPI GetCategory() override { return AIMP_ALBUMART_PROVIDER_CATEGORY_FILE; }
    // IAIMPExtensionAlbumArtProvider2
    HRESULT WINAPI Get2(IAIMPFileInfo* FileInfo, IAIMPPropertyList* Options,
                        IAIMPImageContainer** Image) override;

private:
    IAIMPCore* core_ = nullptr;
    volatile LONG ref_ = 1;
};
