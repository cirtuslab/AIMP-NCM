#pragma once
#include "../third_party/aimp_sdk/apiPlugin.h"
#include "../third_party/aimp_sdk/apiCore.h"
#include "../third_party/aimp_sdk/apiMenu.h"
#include "../third_party/aimp_sdk/apiOptions.h"
#include "../third_party/aimp_sdk/apiPlaylists.h"

class AimpNcmPlugin : public IAIMPPlugin, public IAIMPExternalSettingsDialog {
public:
    AimpNcmPlugin();
    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    // IAIMPPlugin
    PWCHAR WINAPI InfoGet(int Index) override;
    DWORD WINAPI InfoGetCategories() override;
    HRESULT WINAPI Initialize(IAIMPCore* Core) override;
    HRESULT WINAPI Finalize() override;
    void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override {}
    // IAIMPExternalSettingsDialog
    void WINAPI Show(HWND ParentWindow) override;

private:
    volatile LONG ref_=1;
    IAIMPCore* core_=nullptr;
    class NcmFileSystem* fs_ = nullptr;
    class NcmOptionsFrame* optionsFrame_ = nullptr;
    class NcmArtworkProvider* art_ = nullptr;
};
