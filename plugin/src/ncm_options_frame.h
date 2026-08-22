#pragma once
#include "config.h"
#include "../third_party/aimp_sdk/apiOptions.h"
#include "../third_party/aimp_sdk/apiCore.h"
#include "../third_party/aimp_sdk/apiObjects.h"
#include "../third_party/aimp_sdk/apiGUI.h"
#include <windows.h>

// 集成到 AIMP 设置 -> 插件 的选项页（AIMP 原生 UI 控件）
class NcmOptionsFrame : public IAIMPOptionsDialogFrame, public IAIMPOptionsDialogFrameKeyboardHelper {
public:
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

    // 供 FrameWndProc 调用的公共方法
    IAIMPString* MakeStr(const wchar_t* s);
    HWND GetHandle();
    void LoadConfig();
    void SaveConfig(bool notify = true);
    void RefreshPlaylists();
    void TestConnection();
    // 应用设置后: 按勾选生成懒加载 m3u8 并导入 AIMP 播放列表「网易云串流」
    void StartSync();

private:
    // 事件处理器
    static void OnProxyToggled(NcmOptionsFrame* self);
    static void OnApiChanged(NcmOptionsFrame* self);
    static void OnCookieChanged(NcmOptionsFrame* self);
    static void OnQualityChanged(NcmOptionsFrame* self);
    static void OnTestClicked(NcmOptionsFrame* self);
    static void OnQrClicked(NcmOptionsFrame* self);
    static void OnRefreshClicked(NcmOptionsFrame* self);
    static void OnTreeChanged(NcmOptionsFrame* self);
    static void OnCacheChanged(NcmOptionsFrame* self){ self->SaveConfig(true); }

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

    static LRESULT CALLBACK FrameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // 后台线程通知用的隐藏消息窗口(自己创建并拥有, 不子类化 AIMP 表单,
    // 避免占用 AIMP 窗口的 GWLP_USERDATA / 句柄重建导致通知丢失)
    HWND NotifWnd();

    volatile LONG ref_ = 1;
    IAIMPCore* core_ = nullptr;
    IAIMPServiceUI* uiSvc_ = nullptr;
    IAIMPServiceOptionsDialog* optSvc_ = nullptr;
    IAIMPUIForm* form_ = nullptr;
    HWND notifWnd_ = nullptr;      // 隐藏消息窗口
    WNDPROC notifPrevProc_ = nullptr; // 其原始窗口过程

    // 控件引用
    IAIMPUICheckBox* chkProxy_ = nullptr;
    IAIMPUIWinControl* eApi_ = nullptr;
    IAIMPUIButton* btnTest_ = nullptr;
    IAIMPUIWinControl* eCookie_ = nullptr;
    IAIMPUIButton* btnQr_ = nullptr;
    IAIMPUIWinControl* st_ = nullptr;
    IAIMPUIWinControl* cbo_ = nullptr;
    IAIMPUIButton* btnRefresh_ = nullptr;
    IAIMPUIWinControl* lblCnt_ = nullptr;
    IAIMPUITreeList* lst_ = nullptr;
    IAIMPUIWinControl* cboCache_ = nullptr;   // 缓存保留时长下拉
    IAIMPUIWinControl* eCacheWL_ = nullptr;   // 白名单歌单ID输入框

    bool loading_ = false;
};
