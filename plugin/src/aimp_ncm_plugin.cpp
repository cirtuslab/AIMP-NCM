#include "aimp_ncm_plugin.h"
#include "ui_dialog.h"
#include "config.h"
#include "filesystem.h"
#include "local_server.h"
#include "artwork_provider.h"
#include "ncm_options_frame.h"

AimpNcmPlugin::AimpNcmPlugin(){}

HRESULT AimpNcmPlugin::QueryInterface(REFIID riid, void** ppv){
    if(!ppv) return E_POINTER;
    *ppv=nullptr;
    if(IsEqualIID(riid, IID_IUnknown)) *ppv=static_cast<IUnknown*>(static_cast<IAIMPPlugin*>(this));
    else if(IsEqualIID(riid, IID_IAIMPExternalSettingsDialog)) *ppv=static_cast<IAIMPExternalSettingsDialog*>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
}
ULONG AimpNcmPlugin::AddRef(){ return InterlockedIncrement(&ref_); }
ULONG AimpNcmPlugin::Release(){ ULONG c=InterlockedDecrement(&ref_); if(c==0) delete this; return c; }

PWCHAR AimpNcmPlugin::InfoGet(int Index){
    switch(Index){
    case AIMP_PLUGIN_INFO_NAME: return (PWCHAR)L"AIMP NCM 网易云串流";
    case AIMP_PLUGIN_INFO_AUTHOR: return (PWCHAR)L"cirtuslab / YuzuBD";
    case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return (PWCHAR)L"串流网易云歌单到 AIMP 播放列表 [v1.3 ncm-fs]";
    case AIMP_PLUGIN_INFO_FULL_DESCRIPTION: return (PWCHAR)L"支持扫码登录、音质选择、歌单同步，基于 NeteaseCloudMusicApi。需配置镜像 http://localhost:3000";
    default: return nullptr;
    }
}
DWORD AimpNcmPlugin::InfoGetCategories(){ return AIMP_PLUGIN_CATEGORY_ADDONS; }

HRESULT AimpNcmPlugin::Initialize(IAIMPCore* Core){
    if(!Core) return E_INVALIDARG;
    core_=Core;
    // 注册 ncm:// 文件系统 (已修复线程安全)
    // 注意: AIMP 的 RegisterExtension 不持有引用，扩展对象必须由插件自己
    // 持有直到 Finalize()，注册后绝不能 Release()（否则 AIMP 持有悬空指针，
    // 打开设置页/访问文件系统时崩溃）。
    try {
        fs_ = new NcmFileSystem(core_);
        // 文件系统扩展注册点: 参考官方生态(AIMPYouTube)必须用 IID_IAIMPServiceFileSystems,
        // 标签信息提供者用 IID_IAIMPServiceFileInfo
        HRESULT hr1 = core_->RegisterExtension(IID_IAIMPServiceFileSystems, static_cast<IAIMPExtensionFileSystem*>(fs_));
        HRESULT hr2 = core_->RegisterExtension(IID_IAIMPServiceFileInfo, static_cast<IAIMPExtensionFileInfoProvider*>(fs_));
        { extern void FsLog(const char*); char b[96];
          sprintf_s(b, "RegisterExtension FS hr1=0x%08X FileInfo hr2=0x%08X", (unsigned)hr1, (unsigned)hr2);
          FsLog(b); }
    } catch(...){ fs_=nullptr; }
    // 启动本地重定向服务: 播放列表使用 http://127.0.0.1:{port}/pid/tid.mp3 条目,
    // 播放时本插件实时解析真实链接并 302 重定向 (AIMP 原生支持 http, 规避自定义协议问题)
    try {
        extern void FsLog(const char*);
        NcmConfig cfg; ConfigManager::Load(cfg);
        int bound = 0;
        if(LocalServer::Start(cfg.localPort > 0 ? cfg.localPort : 47777, &bound)){
            char b[96]; sprintf_s(b, "LocalServer listening on 127.0.0.1:%d", bound);
            FsLog(b);
            if(bound != cfg.localPort){ cfg.localPort = bound; ConfigManager::Save(cfg); }
        } else {
            FsLog("LocalServer start FAILED");
        }
    } catch(...){}
    // 注册设置对话框（旧入口保留）
    core_->RegisterExtension(IID_IAIMPExternalSettingsDialog, static_cast<IAIMPExternalSettingsDialog*>(this));
    // 注册封面提供者: 为本地代理条目提供网易云歌曲封面
    try {
        art_ = new NcmArtworkProvider(core_);
        core_->RegisterExtension(IID_IAIMPServiceAlbumArt, static_cast<IAIMPExtensionAlbumArtProvider2*>(art_));
    } catch(...){ art_=nullptr; }
    // 注册选项页 (新 SDK 原生) - 集成到 设置->插件 左树
    // 同样: 不 Release，由插件持有至 Finalize()
    try {
        optionsFrame_ = new NcmOptionsFrame(core_);
        core_->RegisterExtension(IID_IAIMPServiceOptionsDialog, static_cast<IAIMPOptionsDialogFrame*>(optionsFrame_));
    } catch(...){ optionsFrame_=nullptr; }
    return S_OK;
}
HRESULT AimpNcmPlugin::Finalize(){
    // 先停本地服务(其工作线程会用到配置/网络)
    LocalServer::Stop();
    if(core_){
        core_->UnregisterExtension(static_cast<IAIMPExternalSettingsDialog*>(this));
        if(optionsFrame_){
            // 注销选项页扩展，然后释放插件持有的引用
            core_->UnregisterExtension(static_cast<IAIMPOptionsDialogFrame*>(optionsFrame_));
            optionsFrame_->Release();
            optionsFrame_=nullptr;
        }
        if(fs_){
            // 注销文件系统扩展，然后释放插件持有的引用
            core_->UnregisterExtension(static_cast<IAIMPExtensionFileSystem*>(fs_));
            core_->UnregisterExtension(static_cast<IAIMPExtensionFileInfoProvider*>(fs_));
            fs_->Release();
            fs_=nullptr;
        }
        if(art_){
            core_->UnregisterExtension(static_cast<IAIMPExtensionAlbumArtProvider2*>(art_));
            art_->Release();
            art_=nullptr;
        }
        core_=nullptr;
    }
    return S_OK;
}
void AimpNcmPlugin::Show(HWND ParentWindow){
    ShowNcmSettingsDialog(ParentWindow, core_);
}
