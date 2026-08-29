#pragma once
#include "config.h"
#include "ncm_client.h"
#include "../third_party/aimp_sdk/apiOptions.h"
#include "../third_party/aimp_sdk/apiCore.h"
#include "../third_party/aimp_sdk/apiObjects.h"
#include "../third_party/aimp_sdk/apiGUI.h"
#include <windows.h>

// 集成到 AIMP 设置 -> 插件 的选项页（AIMP 原生 UI 控件）
class NcmOptionsFrame : public IAIMPOptionsDialogFrame, public IAIMPOptionsDialogFrameKeyboardHelper {
public:
    // AIMP 选项页通知 ID(替代魔法数字 0x1/0x3)
    enum NotifyId {
        NotifyRefresh = 1,   // 选项页打开/刷新时通知
        NotifyApply   = 3,   // 用户点「应用」时通知
    };

    NcmOptionsFrame(IAIMPCore* core);
    ~NcmOptionsFrame();
    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override;
    ULONG WINAPI AddRef() override;
    ULONG WINAPI Release() override;
    // IAIMPOptionsDialogFrame
    HRESULT WINAPI GetName(IAIMPString** S) override;
    HWND WINAPI CreateFrame(HWND ParentWnd) override;
    void WINAPI DestroyFrame() override;
    void WINAPI Notification(int ID) override;
    // KeyboardHelper
    BOOL WINAPI DialogChar(WCHAR CharCode, int Unused) override { return FALSE; }
    BOOL WINAPI DialogKey(WORD CharCode, int Unused) override { return FALSE; }
    BOOL WINAPI SelectFirstControl() override { return FALSE; }
    BOOL WINAPI SelectNextControl(BOOL FindForward, BOOL CheckTabStop) override { return FALSE; }

    // 供后台任务回传后主线程直接调用的公共方法
    IAIMPString* MakeStr(const wchar_t* s);
    HWND GetHandle();
    void LoadConfig();
    void SaveConfig(bool notify = true);
    void RefreshPlaylists();
    void TestConnection();
    // 应用设置后: 按勾选生成懒加载 m3u8 并导入 AIMP 播放列表「网易云串流」
    void StartSync();
    // B1: 主线程辅助(仅主线程调用, 供后台任务结果落地)
    void ShowStatus(const std::wstring& s);
    void ApplyPlaylists(const std::vector<NcmPlaylist>& pls);
    void ImportPlaylist(const std::wstring& m3u, int total);

private:
    // 事件处理器
    static void OnProxyToggled(NcmOptionsFrame* self);
    static void OnApiChanged(NcmOptionsFrame* self);
    static void OnCookieChanged(NcmOptionsFrame* self);
    static void OnQualityChanged(NcmOptionsFrame* self);
    static void OnTestClicked(NcmOptionsFrame* self);
    static void OnRefreshClicked(NcmOptionsFrame* self);
    static void OnTreeChanged(NcmOptionsFrame* self);
    static void OnCacheChanged(NcmOptionsFrame* self){ self->SaveConfig(true); }
    static void OnLyricChanged(NcmOptionsFrame* self){ self->SaveConfig(true); }

    // 遍历歌单树, 把勾选节点的 TAG 收集进 cfg.selectedPlaylists
    void CollectSelection(NcmConfig& cfg);

    // 控件创建辅助
    void* CreateCtl(IAIMPUIWinControl* parent, const wchar_t* name,
        int align, int x, int y, int w, int h, REFIID iid, void** out);
    IAIMPUIButton* CreateBtn(IAIMPUIWinControl* parent, const wchar_t* name,
        const wchar_t* caption, int align, int x, int y, int w, int h,
        void (*handler)(NcmOptionsFrame*));
    IAIMPUICheckBox* CreateChk(IAIMPUIWinControl* parent, const wchar_t* name,
        const wchar_t* caption, int align, int x, int y, int w, int h,
        void (*handler)(NcmOptionsFrame*));
    // 布局辅助（行容器 + 流式左对齐）
    IAIMPUIWinControl* MakeRow(IAIMPUIWinControl* parent, const wchar_t* name,
        int height, int marginTop, int width);
    void PlaceLeft(IAIMPUIWinControl* ctl, int w, int h, int marginLeft, int marginTop);
    IAIMPUIWinControl* MakeLabel(IAIMPUIWinControl* parent, const wchar_t* name,
        const wchar_t* text, int w, int h, int marginLeft, int marginTop);

    // L4: 原 FrameWndProc/NotifWnd 与隐藏消息窗口(WM_USER+2/3/5 通知通道)已移除 ——
    //     v1.5 起后台任务经 ProgCtx + 进度对话框直接回传结果

    volatile LONG ref_ = 1;
    IAIMPCore* core_ = nullptr;
    IAIMPServiceUI* uiSvc_ = nullptr;
    IAIMPServiceOptionsDialog* optSvc_ = nullptr;
    IAIMPUIForm* form_ = nullptr;

    // 控件引用
    IAIMPUICheckBox* chkProxy_ = nullptr;
    IAIMPUIWinControl* eApi_ = nullptr;
    IAIMPUIButton* btnTest_ = nullptr;
    IAIMPUIWinControl* eCookie_ = nullptr;
    IAIMPUIWinControl* st_ = nullptr;
    IAIMPUIWinControl* cbo_ = nullptr;
    IAIMPUIButton* btnRefresh_ = nullptr;
    IAIMPUIWinControl* lblCnt_ = nullptr;
    IAIMPUITreeList* lst_ = nullptr;
    IAIMPUIWinControl* cboCache_ = nullptr;   // 缓存保留时长下拉
    IAIMPUIWinControl* eCacheWL_ = nullptr;   // 白名单歌单ID输入框
    IAIMPUIWinControl* cboLyric_ = nullptr;   // 歌词注入模式下拉

    bool loading_ = false;
};
