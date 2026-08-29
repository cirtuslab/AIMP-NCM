#include "ncm_options_frame.h"
#include "config.h"
#include "utils.h"
#include "ncm_client.h"
#include "http_client.h"
#include "local_server.h"
#include "filesystem.h"
#include "meta_cache.h"
#include "task_center.h"
#include "../third_party/aimp_sdk/apiGUI.h"
#include "../third_party/aimp_sdk/apiOptions.h"
#include "../third_party/aimp_sdk/apiObjects.h"
#include "../third_party/aimp_sdk/apiPlaylists.h"
#include "../third_party/nlohmann/json.hpp"
#include <thread>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>

// =============================================================
// NcmOptionsFrame - AIMP 原生 UI 选项页
// 所有控件通过 IAIMPServiceUI 创建，背景/字体/间距自动跟随 AIMP 皮肤
// =============================================================

// ------- 事件处理（按钮/勾选/编辑变化） -------
// 一个 ChangeEvents 对象，通过回调分发到 NcmOptionsFrame
namespace {
    struct CtlEvents : public IAIMPUIChangeEvents {
        NcmOptionsFrame* self = nullptr;
        void (*handler)(NcmOptionsFrame*) = nullptr;
        CtlEvents(NcmOptionsFrame* s, void (*h)(NcmOptionsFrame*)) : self(s), handler(h) {}
        HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
            if(!ppv) return E_POINTER;
            if(IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAIMPUIChangeEvents)){
                *ppv = static_cast<IAIMPUIChangeEvents*>(this); AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        ULONG WINAPI AddRef() override { return InterlockedIncrement(&ref); }
        ULONG WINAPI Release() override { LONG r = InterlockedDecrement(&ref); if(r==0) delete this; return r; }
        void WINAPI OnChanged(IUnknown*) override { if(self && handler) handler(self); }
        volatile LONG ref = 1;
    };

    // TreeList 专用事件: 复选框勾选走 IAIMPUITreeListEvents::OnNodeChecked,
    // 仅注册 IAIMPUIChangeEvents::OnChanged 时勾选不会回调,
    // 导致"拉取歌单后勾选无法点亮应用按钮"。此处同时实现两类事件。
    struct TreeEvents : public IAIMPUITreeListEvents, public IAIMPUIChangeEvents {
        NcmOptionsFrame* self = nullptr;
        void (*handler)(NcmOptionsFrame*) = nullptr;
        TreeEvents(NcmOptionsFrame* s, void (*h)(NcmOptionsFrame*)) : self(s), handler(h) {}
        HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
            if(!ppv) return E_POINTER;
            if(IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAIMPUITreeListEvents))
                *ppv = static_cast<IAIMPUITreeListEvents*>(this);
            else if(IsEqualIID(riid, IID_IAIMPUIChangeEvents))
                *ppv = static_cast<IAIMPUIChangeEvents*>(this);
            else { *ppv = nullptr; return E_NOINTERFACE; }
            AddRef(); return S_OK;
        }
        ULONG WINAPI AddRef() override { return InterlockedIncrement(&ref); }
        ULONG WINAPI Release() override { LONG r = InterlockedDecrement(&ref); if(r==0) delete this; return r; }
        void WINAPI OnChanged(IUnknown*) override { if(self && handler) handler(self); }
        void WINAPI OnColumnClick(IAIMPUITreeList*, int) override {}
        void WINAPI OnFocusedColumnChanged(IAIMPUITreeList*) override {}
        void WINAPI OnFocusedNodeChanged(IAIMPUITreeList*) override {}
        void WINAPI OnNodeChecked(IAIMPUITreeList*, IAIMPUITreeListNode*) override { if(self && handler) handler(self); }
        void WINAPI OnNodeDblClicked(IAIMPUITreeList*, IAIMPUITreeListNode*) override {}
        void WINAPI OnSelectionChanged(IAIMPUITreeList*) override {}
        void WINAPI OnSorted(IAIMPUITreeList*) override {}
        void WINAPI OnStructChanged(IAIMPUITreeList*) override {}
        volatile LONG ref = 1;
    };

    // ---- B1: AIMP 皮肤进度对话框上下文 ----
    struct ProgCtx {
        HANDLE done = nullptr;      // worker 结束后置位
        HANDLE progress = nullptr;  // 有进度更新
        std::mutex mtx;
        INT64 progPos = 0, progTotal = 0;
        std::wstring progText;
        std::atomic<bool> cancel{false};
        int resultCode = 0;         // 0=ok, 1=错误, 2=取消
        std::wstring resultMsg;
        // StartSync 结果
        std::wstring m3uPath; int total = 0; int failed = 0;
        // RefreshPlaylists 结果
        std::vector<NcmPlaylist> playlists;
    };

    void ProgSet(ProgCtx* ctx, INT64 pos, INT64 total, const std::wstring& text){
        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            ctx->progPos = pos; ctx->progTotal = total; ctx->progText = text;
        }
        SetEvent(ctx->progress);
    }

    struct ProgressEvents : public IAIMPUIProgressDialogEvents {
        ProgCtx* ctx = nullptr;
        explicit ProgressEvents(ProgCtx* c) : ctx(c) {}
        HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
            if(!ppv) return E_POINTER;
            if(IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAIMPUIProgressDialogEvents)){
                *ppv = static_cast<IAIMPUIProgressDialogEvents*>(this); AddRef(); return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        ULONG WINAPI AddRef() override { return InterlockedIncrement(&ref); }
        ULONG WINAPI Release() override { LONG r = InterlockedDecrement(&ref); if(r==0) delete this; return r; }
        void WINAPI OnCanceled() override { if(ctx) ctx->cancel.store(true); }
        volatile LONG ref = 1;
    };

    // 显示 AIMP 皮肤进度对话框并泵消息等待 worker 结束(仅主线程调用)
    void RunProgressDialog(NcmOptionsFrame* self, IAIMPCore* core, ProgCtx* ctx){
        const ULONGLONG kMinShowMs = 2500;   // 最短显示时间: 快速任务(<2.5s)也不闪一下
        IAIMPServiceUI* uiSvc = nullptr;
        if(!core || FAILED(core->QueryInterface(IID_IAIMPServiceUI, (void**)&uiSvc)) || !uiSvc){
            WaitForSingleObject(ctx->done, 60000);   // 无 UI 服务: 退化为直接等待
            return;
        }
        ProgressEvents* ev = new ProgressEvents(ctx);
        IAIMPUIProgressDialog* dlg = nullptr;
        HRESULT hr = uiSvc->CreateObject(nullptr, ev, IID_IAIMPUIProgressDialog, (void**)&dlg);
        ev->Release();   // AIMP 持有引用
        if(FAILED(hr) || !dlg){
            uiSvc->Release();
            WaitForSingleObject(ctx->done, 60000);
            return;
        }
        dlg->SetValueAsObject(AIMPUI_PROGRESSDLG_PROPID_CAPTION, self->MakeStr(L"AIMP NCM"));
        dlg->SetValueAsObject(AIMPUI_PROGRESSDLG_PROPID_MESSAGE, self->MakeStr(L"正在执行，请稍候..."));
        dlg->SetValueAsInt32(AIMPUI_PROGRESSDLG_PROPID_SHOW_PROGRESS_ON_TASKBAR, 1);
        dlg->Started();
        ULONGLONG showStart = GetTickCount64();
        for(;;){
            ULONGLONG elapsed = GetTickCount64() - showStart;
            ULONGLONG remaining = elapsed >= kMinShowMs ? 0 : kMinShowMs - elapsed;
            HANDLE h[2] = { ctx->done, ctx->progress };
            DWORD r = MsgWaitForMultipleObjects(2, h, FALSE,
                                                remaining ? (DWORD)remaining : INFINITE,
                                                QS_ALLINPUT);
            if(r == WAIT_OBJECT_0){
                // worker 完成: 取消时立即关; 否则等满最短显示时间再自动关闭
                if(ctx->cancel.load() || remaining == 0) break;
                continue;
            }
            if(r == WAIT_TIMEOUT) continue;                     // 最短显示时间已到, 继续等 done
            if(r == WAIT_OBJECT_0 + 1){
                std::wstring text; INT64 pos = 0, total = 0;
                {
                    std::lock_guard<std::mutex> lk(ctx->mtx);
                    pos = ctx->progPos; total = ctx->progTotal; text = ctx->progText;
                }
                IAIMPString* ts = self->MakeStr(text.c_str());
                dlg->Progress(pos, total, ts);                  // 主线程调用
                if(ts) ts->Release();
            }
            // 泵消息: 让 AIMP 处理进度对话框的取消事件等
            MSG m;
            while(PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)){
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        dlg->Finished();
        dlg->Release();
        uiSvc->Release();
    }
}

NcmOptionsFrame::NcmOptionsFrame(IAIMPCore* core): core_(core) {
    if(core_) core_->AddRef();
    if(core_) core_->QueryInterface(IID_IAIMPServiceUI, (void**)&uiSvc_);
    if(core_) core_->QueryInterface(IID_IAIMPServiceOptionsDialog, (void**)&optSvc_);
}

NcmOptionsFrame::~NcmOptionsFrame() {
    DestroyFrame();
    if(uiSvc_) { uiSvc_->Release(); uiSvc_=nullptr; }
    if(optSvc_) { optSvc_->Release(); optSvc_=nullptr; }
    if(core_) { core_->Release(); core_=nullptr; }
}

HRESULT NcmOptionsFrame::QueryInterface(REFIID riid, void** ppv){
    if(!ppv) return E_POINTER;
    *ppv=nullptr;
    if(IsEqualIID(riid, IID_IUnknown)) *ppv=static_cast<IUnknown*>(static_cast<IAIMPOptionsDialogFrame*>(this));
    else if(IsEqualIID(riid, IID_IAIMPOptionsDialogFrame)) *ppv=static_cast<IAIMPOptionsDialogFrame*>(this);
    else if(IsEqualIID(riid, IID_IAIMPOptionsDialogFrameKeyboardHelper)) *ppv=static_cast<IAIMPOptionsDialogFrameKeyboardHelper*>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
}
ULONG NcmOptionsFrame::AddRef(){ return InterlockedIncrement(&ref_); }
ULONG NcmOptionsFrame::Release(){ ULONG c=InterlockedDecrement(&ref_); if(c==0) delete this; return c; }

// ---------- 工具 ----------

IAIMPString* NcmOptionsFrame::MakeStr(const wchar_t* s){
    if(!core_) return nullptr;
    IAIMPString* str=nullptr;
    if(SUCCEEDED(core_->CreateObject(IID_IAIMPString, (void**)&str)) && str){
        str->SetData((TChar*)s, (int)wcslen(s));
    }
    return str;
}

// 创建控件并设置布局（Alignment: ualTop/ualLeft/ualClient...）
void* NcmOptionsFrame::CreateCtl(IAIMPUIWinControl* parent, const wchar_t* name,
    int align, int x, int y, int w, int h, REFIID iid, void** out){
    if(!uiSvc_ || !form_ || !parent) return nullptr;
    IAIMPString* nm = MakeStr(name);
    HRESULT hr = uiSvc_->CreateControl(form_, parent, nm, nullptr, iid, out);
    if(nm) nm->Release();
    if(FAILED(hr) || !*out) return nullptr;
    IAIMPUIControl* ctl = (IAIMPUIControl*)*out;
    TAIMPUIControlPlacement pl = {};
    pl.Alignment = (TAIMPUIControlAlignment)align;
    pl.Bounds.left = x; pl.Bounds.top = y;
    pl.Bounds.right = w; pl.Bounds.bottom = h;
    if(align==ualClient){ pl.Bounds.left=0; pl.Bounds.top=0; }
    ctl->SetPlacement(pl);
    return *out;
}

// 创建带 ChangeEvents 的按钮
IAIMPUIButton* NcmOptionsFrame::CreateBtn(IAIMPUIWinControl* parent, const wchar_t* name,
    const wchar_t* caption, int align, int x, int y, int w, int h,
    void (*handler)(NcmOptionsFrame*)){
    IAIMPUIButton* btn=nullptr;
    IAIMPString* nm = MakeStr(name);
    CtlEvents* ev = new CtlEvents(this, handler);
    HRESULT hr = uiSvc_->CreateControl(form_, parent, nm, (IUnknown*)ev, IID_IAIMPUIButton, (void**)&btn);
    if(nm) nm->Release();
    ev->Release(); // AIMP 持有引用
    if(FAILED(hr) || !btn) return nullptr;
    btn->SetValueAsObject(AIMPUI_BUTTON_PROPID_CAPTION, MakeStr(caption));
    TAIMPUIControlPlacement pl = {};
    pl.Alignment = (TAIMPUIControlAlignment)align;
    pl.Bounds.left=x; pl.Bounds.top=y; pl.Bounds.right=w; pl.Bounds.bottom=h;
    ((IAIMPUIControl*)btn)->SetPlacement(pl);
    return btn;
}

// 创建带 ChangeEvents 的 CheckBox
IAIMPUICheckBox* NcmOptionsFrame::CreateChk(IAIMPUIWinControl* parent, const wchar_t* name,
    const wchar_t* caption, int align, int x, int y, int w, int h,
    void (*handler)(NcmOptionsFrame*)){
    IAIMPUICheckBox* chk=nullptr;
    IAIMPString* nm = MakeStr(name);
    CtlEvents* ev = new CtlEvents(this, handler);
    HRESULT hr = uiSvc_->CreateControl(form_, parent, nm, (IUnknown*)ev, IID_IAIMPUICheckBox, (void**)&chk);
    if(nm) nm->Release();
    ev->Release();
    if(FAILED(hr) || !chk) return nullptr;
    chk->SetValueAsObject(AIMPUI_CHECKBOX_PROPID_CAPTION, MakeStr(caption));
    TAIMPUIControlPlacement pl = {};
    pl.Alignment = (TAIMPUIControlAlignment)align;
    pl.Bounds.left=x; pl.Bounds.top=y; pl.Bounds.right=w; pl.Bounds.bottom=h;
    ((IAIMPUIControl*)chk)->SetPlacement(pl);
    return chk;
}

// ---------- 布局辅助（参考 aimp_desktop_lyrics 的行容器模式） ----------

// 创建一行容器（ualTop 纵向堆叠），返回行 Panel
IAIMPUIWinControl* NcmOptionsFrame::MakeRow(IAIMPUIWinControl* parent, const wchar_t* name,
    int height, int marginTop, int width){
    if(!uiSvc_ || !form_ || !parent) return nullptr;
    IAIMPUIWinControl* row=nullptr;
    IAIMPString* nm = MakeStr(name);
    HRESULT hr = uiSvc_->CreateControl(form_, parent, nm, nullptr, IID_IAIMPUIPanel, (void**)&row);
    if(nm) nm->Release();
    if(FAILED(hr) || !row) return nullptr;
    row->SetValueAsInt32(AIMPUI_PANEL_PROPID_BORDERS, AIMPUI_FLAGS_BORDERS_NONE);
    TAIMPUIControlPlacement pl={};
    pl.Alignment = ualTop;
    pl.AlignmentMargins.top = marginTop;
    pl.Bounds.right = width; pl.Bounds.bottom = height;
    ((IAIMPUIControl*)row)->SetPlacement(pl);
    return row;
}

// 行内左对齐控件（流式排列，marginLeft 控制间距）
void NcmOptionsFrame::PlaceLeft(IAIMPUIWinControl* ctl, int w, int h, int marginLeft, int marginTop){
    if(!ctl) return;
    TAIMPUIControlPlacement pl={};
    pl.Alignment = ualLeft;
    pl.AlignmentMargins.left = marginLeft;
    pl.AlignmentMargins.top = marginTop;
    pl.AlignmentMargins.bottom = marginTop;
    pl.Bounds.right = w; pl.Bounds.bottom = h;
    ((IAIMPUIControl*)ctl)->SetPlacement(pl);
}

// 组内创建 Label（左对齐）
IAIMPUIWinControl* NcmOptionsFrame::MakeLabel(IAIMPUIWinControl* parent, const wchar_t* name,
    const wchar_t* text, int w, int h, int marginLeft, int marginTop){
    IAIMPUIWinControl* lbl=nullptr;
    CreateCtl(parent, name, ualLeft, 0,0, w,h, IID_IAIMPUILabel, (void**)&lbl);
    if(lbl){
        lbl->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, MakeStr(text));
        PlaceLeft(lbl, w, h, marginLeft, marginTop);
    }
    return lbl;
}

// ---------- 事件处理器 ----------

void NcmOptionsFrame::OnProxyToggled(NcmOptionsFrame* self){ self->SaveConfig(true); }
void NcmOptionsFrame::OnApiChanged(NcmOptionsFrame* self){ self->SaveConfig(true); }
void NcmOptionsFrame::OnCookieChanged(NcmOptionsFrame* self){ self->SaveConfig(true); }
void NcmOptionsFrame::OnQualityChanged(NcmOptionsFrame* self){ self->SaveConfig(true); }
void NcmOptionsFrame::OnTestClicked(NcmOptionsFrame* self){ self->TestConnection(); }

// 歌单树勾选变化: 持久化 + 点亮 AIMP 的"应用"按钮
void NcmOptionsFrame::OnTreeChanged(NcmOptionsFrame* self){
    if(self->loading_) return;
    // D6: 勾选不再即时写盘, 只标记"已修改"点亮应用; 点「应用」时统一保存
    if(self->optSvc_) self->optSvc_->FrameModified(self);
    // 更新已选计数
    if(self->lblCnt_){
        NcmConfig c; ConfigManager::Load(c);
        wchar_t buf[64];
        swprintf_s(buf, L"已选 %zu 个", c.selectedPlaylists.size());
        self->lblCnt_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(buf));
    }
}
void NcmOptionsFrame::OnRefreshClicked(NcmOptionsFrame* self){ self->RefreshPlaylists(); }

// ---------- IAIMPOptionsDialogFrame ----------

HRESULT NcmOptionsFrame::GetName(IAIMPString** S){
    if(!S) return E_POINTER;
    *S = MakeStr(L"网易云串流");
    return *S ? S_OK : E_FAIL;
}

HWND NcmOptionsFrame::CreateFrame(HWND ParentWnd){
    if(!uiSvc_) return nullptr;
    // 创建子窗体（背景/字体跟随 AIMP 皮肤）
    IAIMPString* fn = MakeStr(L"NcmOptions");
    HRESULT hr = uiSvc_->CreateForm(ParentWnd, AIMPUI_SERVICE_CREATEFORM_FLAGS_CHILD, fn, nullptr, &form_);
    if(fn) fn->Release();
    if(FAILED(hr) || !form_) return nullptr;

    // 创建自己的隐藏消息窗口接收后台线程的 WM_USER+2/3 通知。
    // 之前把窗口过程子类化到 AIMP 表单上并占用其 GWLP_USERDATA，
    // AIMP 内部管理该窗口时会使 self 指针失效 -> 通知全部丢失(卡在"正在拉取歌单")
    {
        static ATOM cls = 0;
        if(!cls){
            WNDCLASSW wc = {};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(L"aimp_ncm.dll");
            wc.lpszClassName = L"AIMP_NCM_NotifWnd";
            cls = RegisterClassW(&wc);
        }
        notifWnd_ = CreateWindowExW(0, L"AIMP_NCM_NotifWnd", L"", 0, 0,0,0,0, HWND_MESSAGE, nullptr, GetModuleHandleW(L"aimp_ncm.dll"), nullptr);
        if(notifWnd_){
            SetWindowLongPtrW(notifWnd_, GWLP_USERDATA, (LONG_PTR)this);
            notifPrevProc_ = (WNDPROC)SetWindowLongPtrW(notifWnd_, GWLP_WNDPROC, (LONG_PTR)NcmOptionsFrame::FrameWndProc);
        }
    }

    // 根容器：占满整个页面
    IAIMPUIWinControl* root=nullptr;
    CreateCtl(form_, L"root", ualClient, 0,0, 0,0, IID_IAIMPUIPanel, (void**)&root);
    if(!root) return nullptr;
    TAIMPUIControlPlacement plRoot={}; plRoot.Alignment=ualClient;
    ((IAIMPUIControl*)root)->SetPlacement(plRoot);

    // ============ 连接与登录 分组（AUTOSIZE 自适应内容高度） ============
    IAIMPUIWinControl* grpConn=nullptr;
    CreateCtl(root, L"grpConn", ualTop, 0,0, 0,0, IID_IAIMPUIGroupBox, (void**)&grpConn);
    if(grpConn){
        grpConn->SetValueAsObject(AIMPUI_GROUPBOX_PROPID_CAPTION, MakeStr(L"连接与登录"));
        grpConn->SetValueAsInt32(AIMPUI_GROUPBOX_PROPID_AUTOSIZE, 1);
        TAIMPUIControlPlacement pl={}; pl.Alignment=ualTop;
        ((IAIMPUIControl*)grpConn)->SetPlacement(pl);

        // ---- 行1: 代理开关 ----
        IAIMPUIWinControl* rowProxy = MakeRow(grpConn, L"rowProxy", 22, 16, 420);
        if(rowProxy){
            chkProxy_ = CreateChk(rowProxy, L"chkProxy", L"启用代理（海外/被封时勾选）", ualLeft, 0,0, 220,20, OnProxyToggled);
            if(chkProxy_) PlaceLeft(chkProxy_, 220, 20, 0, 1);
        }

        // ---- 行2: 镜像地址 + 测试按钮 ----
        IAIMPUIWinControl* rowApi = MakeRow(grpConn, L"rowApi", 24, 8, 420);
        if(rowApi){
            IAIMPUIWinControl* lblApi = MakeLabel(rowApi, L"lblApi", L"镜像地址:", 62, 20, 0, 2);
            // 地址输入框
            {
                CtlEvents* ev = new CtlEvents(this, OnApiChanged);
                IAIMPString* nm = MakeStr(L"eApi");
                hr = uiSvc_->CreateControl(form_, rowApi, nm, (IUnknown*)ev, IID_IAIMPUIEdit, (void**)&eApi_);
                if(nm) nm->Release();
                ev->Release();
                if(SUCCEEDED(hr) && eApi_){
                    eApi_->SetValueAsObject(AIMPUI_EDIT_PROPID_TEXTHINT, MakeStr(L"http://localhost:3000 或 http://你的VPS:3000"));
                    PlaceLeft(eApi_, 250, 22, 8, 1);
                }
            }
            btnTest_ = CreateBtn(rowApi, L"btnTest", L"测试", ualLeft, 0,0, 56,22, OnTestClicked);
            if(btnTest_) PlaceLeft(btnTest_, 56, 22, 8, 1);
        }

        // ---- 行3: Cookie 直填 ----
        IAIMPUIWinControl* rowCookie = MakeRow(grpConn, L"rowCookie", 24, 8, 420);
        if(rowCookie){
            IAIMPUIWinControl* lblCookie = MakeLabel(rowCookie, L"lblCookie", L"Cookie:", 62, 20, 0, 2);
            {
                CtlEvents* ev = new CtlEvents(this, OnCookieChanged);
                IAIMPString* nm = MakeStr(L"eCookie");
                hr = uiSvc_->CreateControl(form_, rowCookie, nm, (IUnknown*)ev, IID_IAIMPUIEdit, (void**)&eCookie_);
                if(nm) nm->Release();
                ev->Release();
                if(SUCCEEDED(hr) && eCookie_){
                    eCookie_->SetValueAsObject(AIMPUI_EDIT_PROPID_PASSWORDCHAR, MakeStr(L"●"));
                    eCookie_->SetValueAsObject(AIMPUI_EDIT_PROPID_TEXTHINT, MakeStr(L"粘贴 MUSIC_U=... (只粘值也会自动补键名)"));
                    PlaceLeft(eCookie_, 250, 22, 8, 1);
                }
            }
        }

        // ---- 行4: 状态 ----
        IAIMPUIWinControl* rowSt = MakeRow(grpConn, L"rowSt", 20, 8, 420);
        if(rowSt){
            st_ = MakeLabel(rowSt, L"st", L"就绪", 400, 18, 0, 1);
        }
    }

    // ============ 播放设置与歌单 分组（占满剩余空间） ============
    IAIMPUIWinControl* grpList=nullptr;
    CreateCtl(root, L"grpList", ualClient, 0,0, 0,0, IID_IAIMPUIGroupBox, (void**)&grpList);
    if(grpList){
        grpList->SetValueAsObject(AIMPUI_GROUPBOX_PROPID_CAPTION, MakeStr(L"播放设置与歌单"));
        IAIMPUIWinControl* body2=nullptr;
        CreateCtl(grpList, L"listBody", ualClient, 0,0, 0,0, IID_IAIMPUIPanel, (void**)&body2);
        if(body2){
            TAIMPUIControlPlacement pl={}; pl.Alignment=ualClient;
            pl.AlignmentMargins.left=12; pl.AlignmentMargins.top=18;
            pl.AlignmentMargins.right=12; pl.AlignmentMargins.bottom=8;
            ((IAIMPUIControl*)body2)->SetPlacement(pl);

            // ---- 行1: 音质 + 刷新按钮 + 提示 ----
            IAIMPUIWinControl* rowQuality = MakeRow(body2, L"rowQuality", 24, 0, 420);
            if(rowQuality){
                IAIMPUIWinControl* lblQuality = MakeLabel(rowQuality, L"lblQuality", L"音质:", 44, 20, 0, 2);
                // 音质下拉
                {
                    CtlEvents* ev = new CtlEvents(this, OnQualityChanged);
                    IAIMPString* nm = MakeStr(L"cboQuality");
                    hr = uiSvc_->CreateControl(form_, rowQuality, nm, (IUnknown*)ev, IID_IAIMPUIComboBox, (void**)&cbo_);
                    if(nm) nm->Release();
                    ev->Release();
                    if(SUCCEEDED(hr) && cbo_){
                        IAIMPUIBaseComboBox* cb=(IAIMPUIBaseComboBox*)cbo_;
                        const wchar_t* names[]={L"标准 128k",L"较高 192k",L"极高 320k",L"无损 FLAC",L"Hi-Res",L"高清臻音",L"沉浸环绕",L"超清母带"};
                        for(int i=0;i<8;i++){
                            IAIMPString* s=MakeStr(names[i]);
                            if(s){ cb->Add(s, 0); s->Release(); }
                        }
                        PlaceLeft(cbo_, 150, 22, 8, 1);
                    }
                }
                btnRefresh_ = CreateBtn(rowQuality, L"btnRefresh", L"刷新歌单", ualLeft, 0,0, 78,22, OnRefreshClicked);
                if(btnRefresh_) PlaceLeft(btnRefresh_, 78, 22, 12, 1);
                IAIMPUIWinControl* hint = MakeLabel(rowQuality, L"hint", L"提示：切换音质实时生效，下次播放起生效", 220, 18, 12, 3);
            }

            // ---- 行1.5: 缓存策略(保留时长 + 白名单歌单) ----
            IAIMPUIWinControl* rowCache = MakeRow(body2, L"rowCache", 24, 8, 420);
            if(rowCache){
                MakeLabel(rowCache, L"lblCache", L"缓存保留:", 62, 20, 0, 2);
                {
                    CtlEvents* ev = new CtlEvents(this, OnCacheChanged);
                    IAIMPString* nm = MakeStr(L"cboCache");
                    hr = uiSvc_->CreateControl(form_, rowCache, nm, (IUnknown*)ev, IID_IAIMPUIComboBox, (void**)&cboCache_);
                    if(nm) nm->Release();
                    ev->Release();
                    if(SUCCEEDED(hr) && cboCache_){
                        IAIMPUIBaseComboBox* cb=(IAIMPUIBaseComboBox*)cboCache_;
                        const wchar_t* items[]={L"永不删除",L"1 天",L"3 天",L"7 天",L"30 天"};
                        for(int i=0;i<5;i++){
                            IAIMPString* s=MakeStr(items[i]);
                            if(s){ cb->Add(s, 0); s->Release(); }
                        }
                        PlaceLeft(cboCache_, 90, 22, 8, 1);
                    }
                }
                MakeLabel(rowCache, L"lblWL", L"白名单歌单ID:", 92, 20, 10, 2);
                {
                    CtlEvents* ev = new CtlEvents(this, OnCacheChanged);
                    IAIMPString* nm = MakeStr(L"eCacheWL");
                    hr = uiSvc_->CreateControl(form_, rowCache, nm, (IUnknown*)ev, IID_IAIMPUIEdit, (void**)&eCacheWL_);
                    if(nm) nm->Release();
                    ev->Release();
                    if(SUCCEEDED(hr) && eCacheWL_){
                        eCacheWL_->SetValueAsObject(AIMPUI_EDIT_PROPID_TEXTHINT,
                            MakeStr(L"这些歌单的缓存永不删除，逗号分隔"));
                        PlaceLeft(eCacheWL_, 160, 22, 6, 1);
                    }
                }
            }

            // ---- 行1.6: 歌词注入模式 ----
            IAIMPUIWinControl* rowLyric = MakeRow(body2, L"rowLyric", 24, 8, 420);
            if(rowLyric){
                MakeLabel(rowLyric, L"lblLyric", L"歌词注入:", 62, 20, 0, 2);
                {
                    CtlEvents* ev = new CtlEvents(this, OnLyricChanged);
                    IAIMPString* nm = MakeStr(L"cboLyric");
                    hr = uiSvc_->CreateControl(form_, rowLyric, nm, (IUnknown*)ev, IID_IAIMPUIComboBox, (void**)&cboLyric_);
                    if(nm) nm->Release();
                    ev->Release();
                    if(SUCCEEDED(hr) && cboLyric_){
                        IAIMPUIBaseComboBox* cb=(IAIMPUIBaseComboBox*)cboLyric_;
                        const wchar_t* items[]={L"不注入",L"USLT(内嵌歌词)"};
                        for(int i=0;i<2;i++){
                            IAIMPString* s=MakeStr(items[i]);
                            if(s){ cb->Add(s, 0); s->Release(); }
                        }
                        PlaceLeft(cboLyric_, 150, 22, 8, 1);
                    }
                }
                MakeLabel(rowLyric, L"hintLyric", L"USLT 已验证支持 mp3/flac/wav；首次播放拉取后缓存", 300, 18, 12, 3);
            }

            // ---- 行2: 歌单标题 + 计数 ----
            IAIMPUIWinControl* rowTitle = MakeRow(body2, L"rowTitle", 20, 8, 420);
            if(rowTitle){
                IAIMPUIWinControl* lblList = MakeLabel(rowTitle, L"lblList", L"歌单（勾选后点“应用”保存）", 200, 18, 0, 1);
                lblCnt_ = MakeLabel(rowTitle, L"lblCnt", L"尚未选择", 140, 18, 20, 1);
            }

            // ---- 行3: 歌单 TreeList（占满剩余空间） ----
            {
                // 挂接勾选变化事件: 点亮"应用"并即时保存
                // 必须用 TreeEvents(含 OnNodeChecked), 仅 OnChanged 无法感知复选框
                TreeEvents* ev = new TreeEvents(this, OnTreeChanged);
                IAIMPString* nm = MakeStr(L"lstPlaylists");
                // TreeEvents 多继承两个事件接口, 到 IUnknown 需先显式走一个基类
                hr = uiSvc_->CreateControl(form_, body2, nm,
                                           (IUnknown*)(IAIMPUITreeListEvents*)ev,
                                           IID_IAIMPUITreeList, (void**)&lst_);
                if(nm) nm->Release();
                ev->Release();
                if(SUCCEEDED(hr) && lst_){
                    lst_->SetValueAsInt32(AIMPUI_TL_PROPID_ALLOW_MULTISELECT, 1);
                    lst_->SetValueAsInt32(AIMPUI_TL_PROPID_CHECKBOXES, 1);
                    lst_->SetValueAsInt32(AIMPUI_TL_PROPID_ALLOW_EDITING, 0);
                    lst_->SetValueAsInt32(AIMPUI_TL_PROPID_DRAG_SORTING, 0);
                    lst_->SetValueAsInt32(AIMPUI_TL_PROPID_ALLOW_DELETING, 0);
                    // 添加一列
                    IAIMPUITreeListColumn* col=nullptr;
                    if(SUCCEEDED(lst_->AddColumn(IID_IAIMPUITreeListColumn, (void**)&col)) && col){
                        col->SetValueAsObject(AIMPUI_TL_COLUMN_PROPID_CAPTION, MakeStr(L"歌单"));
                        col->SetValueAsInt32(AIMPUI_TL_COLUMN_PROPID_WIDTH, 400);
                        col->Release();
                    }
                    // ualClient 占满剩余空间，行容器之上
                    TAIMPUIControlPlacement pl={}; pl.Alignment=ualClient;
                    pl.AlignmentMargins.top=4;
                    ((IAIMPUIControl*)lst_)->SetPlacement(pl);
                }
            }
        }
    }

    LoadConfig();
    return ((IAIMPUIWinControl*)form_)->GetHandle();
}

HWND NcmOptionsFrame::NotifWnd(){
    // 优先用自有消息窗口; 不可用时回退表单句柄
    if(notifWnd_ && IsWindow(notifWnd_)) return notifWnd_;
    return GetHandle();
}

void NcmOptionsFrame::DestroyFrame(){
    // 先销毁自有消息窗口(WM_NCDESTROY 中自动还原窗口过程)
    if(notifWnd_){
        HWND w = notifWnd_; notifWnd_ = nullptr;
        if(IsWindow(w)) DestroyWindow(w);
        notifPrevProc_ = nullptr;
    }
    if(chkProxy_){ chkProxy_->Release(); chkProxy_=nullptr; }
    if(eApi_){ eApi_->Release(); eApi_=nullptr; }
    if(btnTest_){ btnTest_->Release(); btnTest_=nullptr; }
    if(eCookie_){ eCookie_->Release(); eCookie_=nullptr; }
    if(st_){ st_->Release(); st_=nullptr; }
    if(cbo_){ cbo_->Release(); cbo_=nullptr; }
    if(btnRefresh_){ btnRefresh_->Release(); btnRefresh_=nullptr; }
    if(lblCnt_){ lblCnt_->Release(); lblCnt_=nullptr; }
    if(lst_){ lst_->Release(); lst_=nullptr; }
    if(cboCache_){ cboCache_->Release(); cboCache_=nullptr; }
    if(eCacheWL_){ eCacheWL_->Release(); eCacheWL_=nullptr; }
    if(cboLyric_){ cboLyric_->Release(); cboLyric_=nullptr; }
    if(form_){ ((IUnknown*)form_)->Release(); form_=nullptr; }
}

void NcmOptionsFrame::Notification(int ID){
    if(ID==NotifyApply){
        SaveConfig(true);   // 应用前先落盘勾选状态
        StartSync();
        LocalServer::RunCleanupNow();  // 应用后按新缓存策略立即执行一次清理
    }
    else if(ID==NotifyRefresh) LoadConfig();
}

// ---------- 应用后同步: 勾选歌单 -> 懒加载 m3u8 -> AIMP 播放列表 ----------

void NcmOptionsFrame::StartSync(){
    if(!form_) return;
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(cfg.selectedPlaylists.empty()){
        ShowStatus(L"未勾选任何歌单，跳过同步");
        return;
    }
    ShowStatus(L"正在生成播放列表...");

    ProgCtx* ctx = new ProgCtx();
    ctx->done     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ctx->progress = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // B1: 后台任务由插件跟踪(不捕获 this), 只通过 ctx/对话框通信;
    // AIMP 进度对话框期间设置页不可关闭, 杜绝"关页丢结果"
    TaskCenter::Run([ctx]{
        try{
            NcmConfig c; ConfigManager::Load(c);
            NcmClient client(c);
            WCHAR tmp[MAX_PATH]={0}; GetTempPathW(MAX_PATH, tmp);
            std::wstring dir = std::wstring(tmp) + L"aimp_ncm";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring m3u = dir + L"\\ncm_playlist.m3u8";
            std::ofstream f(m3u.c_str());
            if(!f){
                ctx->resultCode = 1; ctx->resultMsg = L"无法创建临时 m3u8 文件";
                SetEvent(ctx->done); return;
            }
            f << "#EXTM3U\n";
            // D6: 缓冲写 m3u8, 减少逐行磁盘 IO
            std::string m3uBuf;
            auto flushM3u = [&](){
                if(!m3uBuf.empty()){ f << m3uBuf; f.flush(); m3uBuf.clear(); }
            };
            int port = c.localPort > 0 ? c.localPort : 47777;
            std::string base = "http://127.0.0.1:" + std::to_string(port) + "/";
            int total=0, failed=0, plIdx=0, plCount=(int)c.selectedPlaylists.size();
            for(auto pid : c.selectedPlaylists){
                if(ctx->cancel.load() || TaskCenter::IsShuttingDown()){ ctx->resultCode = 2; break; }
                plIdx++;
                ProgSet(ctx, plIdx, plCount,
                        L"正在获取歌单 " + std::to_wstring(plIdx) + L"/" + std::to_wstring(plCount) + L" ...");
                std::vector<NcmSong> songs; NcmPlaylist info;
                if(!client.GetPlaylistDetail(pid, songs, &info) || songs.empty()){ failed++; continue; }
                NcmMeta::WritePlaylist(pid, songs);
                for(auto& s : songs){
                    m3uBuf += "#EXTINF:" + std::to_string(s.durationMs/1000) + "," +
                              WideToUtf8(s.artist) + " - " + WideToUtf8(s.title) + "\n";
                    m3uBuf += base + std::to_string(pid) + "/" + std::to_string(s.id) + "\n";
                    total++;
                    if(m3uBuf.size() >= 64*1024) flushM3u();
                }
            }
            flushM3u();
            f.close();
            if(ctx->resultCode != 2){
                if(total == 0){
                    wchar_t b[128];
                    swprintf_s(b, L"未获取到歌曲(失败 %d 个歌单) · 请检查登录状态/网络后重试", failed);
                    ctx->resultCode = 1; ctx->resultMsg = b;
                } else {
                    ctx->resultCode = 0; ctx->m3uPath = m3u; ctx->total = total; ctx->failed = failed;
                }
            }
        }catch(...){
            ctx->resultCode = 1; ctx->resultMsg = L"同步线程发生异常";
            FsLog("StartSync worker exception");
        }
        SetEvent(ctx->done);
    });

    RunProgressDialog(this, core_, ctx);
    if(ctx->cancel.load()){
        ShowStatus(L"已取消同步");
    } else if(ctx->resultCode == 0){
        ImportPlaylist(ctx->m3uPath, ctx->total);
    } else {
        ShowStatus(ctx->resultMsg.empty() ? L"同步失败" : ctx->resultMsg);
    }
    CloseHandle(ctx->progress); CloseHandle(ctx->done);
    delete ctx;
}

// ---------- B1: 主线程辅助(仅主线程调用) ----------

void NcmOptionsFrame::ShowStatus(const std::wstring& s){
    if(st_) st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, MakeStr(s.c_str()));
}

void NcmOptionsFrame::ApplyPlaylists(const std::vector<NcmPlaylist>& pls){
    if(!lst_) return;
    loading_ = true;  // 程序化填充期间屏蔽 OnTreeChanged
    lst_->Clear();
    IAIMPUITreeListNode* root = nullptr;
    if(SUCCEEDED(lst_->GetRootNode(IID_IAIMPUITreeListNode, (void**)&root)) && root){
        NcmConfig cfgSel; ConfigManager::Load(cfgSel);
        for(auto& pl : pls){
            std::wstring txt = pl.name + L"  ·  " + std::to_wstring(pl.trackCount) + L"首";
            if(!pl.creator.empty()) txt += L"  ·  " + pl.creator;
            IAIMPString* s = MakeStr(txt.c_str());
            if(s){
                IAIMPUITreeListNode* node = nullptr;
                if(SUCCEEDED(root->Add(&node)) && node){
                    node->SetValue(0, s);
                    node->SetValueAsInt64(AIMPUI_TL_NODE_PROPID_TAG, pl.id);
                    bool sel = std::find(cfgSel.selectedPlaylists.begin(),
                                         cfgSel.selectedPlaylists.end(), pl.id) != cfgSel.selectedPlaylists.end();
                    node->SetValueAsInt32(AIMPUI_TL_NODE_PROPID_CHECKED,
                                          sel ? AIMPUI_CHECKSTATE_CHECKED : AIMPUI_CHECKSTATE_UNCHECKED);
                    node->Release();
                }
                s->Release();
            }
        }
        root->Release();
    }
    wchar_t buf[64];
    swprintf_s(buf, L"共 %d 个歌单", (int)pls.size());
    if(lblCnt_) lblCnt_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, MakeStr(buf));
    ShowStatus(L"歌单加载完成，勾选后点“应用”保存");
    SaveConfig(false);
    loading_ = false;
    SaveConfig(false);   // loading_=true 时上一步被跳过, 补一次落盘恢复的勾选状态
}

void NcmOptionsFrame::ImportPlaylist(const std::wstring& m3u, int total){
    if(!core_) return;
    ShowStatus(L"正在导入播放列表...");
    IAIMPServicePlaylistManager* pm = nullptr;
    if( SUCCEEDED(core_->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&pm))
     || SUCCEEDED(core_->CreateObject(IID_IAIMPServicePlaylistManager, (void**)&pm)) ){
        if(pm){
            IAIMPString* name = MakeStr(L"网易云串流");
            IAIMPPlaylist* pl = nullptr;
            if(name){
                if(FAILED(pm->GetLoadedPlaylistByName(name, &pl)) || !pl)
                    pm->CreatePlaylist(name, TRUE, &pl);
            }
            if(pl){
                IAIMPString* path = MakeStr(m3u.c_str());
                HRESULT hrAdd = E_FAIL;
                if(path){
                    pl->BeginUpdate();
                    pl->DeleteAll();
                    hrAdd = pl->Add(path, 0, -1);
                    pl->EndUpdate();
                    path->Release();
                }
                wchar_t b[128];
                if(SUCCEEDED(hrAdd)) swprintf_s(b, L"已同步 %d 首到播放列表「网易云串流」(本地代理, 播放时实时取链)", total);
                else swprintf_s(b, L"同步失败 (Add 0x%08X)", (unsigned)hrAdd);
                ShowStatus(b);
            } else {
                ShowStatus(L"无法创建/找到播放列表");
            }
            if(name) name->Release();
            if(pl) pl->Release();
        }
    } else {
        ShowStatus(L"获取 PlaylistManager 服务失败");
    }
    if(pm) pm->Release();
}

// ---------- 配置加载/保存 ----------

void NcmOptionsFrame::LoadConfig(){
    if(!form_) return;
    loading_ = true;
    NcmConfig cfg; ConfigManager::Load(cfg);
    if(chkProxy_) chkProxy_->SetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, cfg.useProxy?AIMPUI_CHECKSTATE_CHECKED:AIMPUI_CHECKSTATE_UNCHECKED);
    if(eApi_ && !cfg.apiUrl.empty()) eApi_->SetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, MakeStr(cfg.apiUrl.c_str()));
    if(eCookie_ && !cfg.cookie.empty()) eCookie_->SetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, MakeStr(cfg.cookie.c_str()));
    if(cbo_){
        IAIMPUIBaseComboBox* cb=(IAIMPUIBaseComboBox*)cbo_;
        const wchar_t* levels[]={L"standard",L"higher",L"exhigh",L"lossless",L"hires",L"jymaster",L"jyeffect",L"sky"};
        int sel=2; for(int i=0;i<8;i++) if(cfg.quality==levels[i]) sel=i;
        cb->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, sel);
    }
    // 缓存策略
    {
        static const int days[5] = { -1, 1, 3, 7, 30 };
        int idx = 3; // 默认 7 天
        for(int i=0;i<5;i++) if(cfg.cacheDays==days[i]) idx=i;
        if(cboCache_){
            IAIMPUIBaseComboBox* cb=(IAIMPUIBaseComboBox*)cboCache_;
            cb->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, idx);
        }
        if(eCacheWL_ && !cfg.cacheWhitelist.empty())
            eCacheWL_->SetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, MakeStr(cfg.cacheWhitelist.c_str()));
        if(cboLyric_){
            IAIMPUIBaseComboBox* cb=(IAIMPUIBaseComboBox*)cboLyric_;
            cb->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, cfg.lyricMode == L"none" ? 0 : 1);
        }
    }
    if(st_){
        std::wstring s = L"就绪";
        if(!cfg.cookie.empty()) s = L"已登录 · Cookie 已保存";
        else s = L"未登录 · 请粘贴 Cookie";
        if(cfg.useProxy && !cfg.apiUrl.empty()) s += L"  · 代理: " + cfg.apiUrl;
        else if(!cfg.useProxy) s += L"  · 直连 music.163.com";
        st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, MakeStr(s.c_str()));
    }
    if(lblCnt_){
        if(!cfg.selectedPlaylists.empty()){
            wchar_t buf[64]; swprintf_s(buf, L"已选 %zu 个", cfg.selectedPlaylists.size());
            lblCnt_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, MakeStr(buf));
        } else lblCnt_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, MakeStr(L"尚未选择"));
    }
    loading_ = false;
}

void NcmOptionsFrame::CollectSelection(NcmConfig& cfg){
    cfg.selectedPlaylists.clear();
    if(!lst_) return;
    IAIMPUITreeListNode* root=nullptr;
    if(SUCCEEDED(lst_->GetRootNode(IID_IAIMPUITreeListNode, (void**)&root)) && root){
        int count = root->GetCount();
        for(int i=0;i<count;i++){
            IAIMPUITreeListNode* node=nullptr;
            if(SUCCEEDED(root->Get(i, IID_IAIMPUITreeListNode, (void**)&node)) && node){
                long long tag=0; node->GetValueAsInt64(AIMPUI_TL_NODE_PROPID_TAG, &tag);
                int chk=0; node->GetValueAsInt32(AIMPUI_TL_NODE_PROPID_CHECKED, &chk);
                if(tag && chk==AIMPUI_CHECKSTATE_CHECKED) cfg.selectedPlaylists.push_back(tag);
                node->Release();
            }
        }
        root->Release();
    }
}

void NcmOptionsFrame::SaveConfig(bool notify){
    if(!form_ || loading_) return;
    NcmConfig cfg; ConfigManager::Load(cfg);
    std::wstring oldCookie = cfg.cookie;
    if(chkProxy_){ int v=0; chkProxy_->GetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, &v); cfg.useProxy=(v==AIMPUI_CHECKSTATE_CHECKED); }
    if(eApi_){
        IAIMPString* s=nullptr;
        eApi_->GetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, IID_IAIMPString, (void**)&s);
        if(s){ cfg.apiUrl = AimpStringToWString(s); s->Release(); }
    }
    if(eCookie_){
        IAIMPString* s=nullptr;
        eCookie_->GetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, IID_IAIMPString, (void**)&s);
        if(s){ cfg.cookie = NormalizeCookie(AimpStringToWString(s)); s->Release(); }
    }
    if(cbo_){
        int sel=0; cbo_->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &sel);
        const wchar_t* levels[]={L"standard",L"higher",L"exhigh",L"lossless",L"hires",L"jymaster",L"jyeffect",L"sky"};
        if(sel>=0&&sel<8) cfg.quality = levels[sel];
    }
    // 缓存策略: 下拉 -> 天数(-1=永不); 白名单歌单ID
    if(cboCache_){
        int idx=3; cboCache_->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &idx);
        static const int days[5] = { -1, 1, 3, 7, 30 };
        if(idx>=0&&idx<5) cfg.cacheDays = days[idx];
        else cfg.cacheDays = 7;
    }
    if(eCacheWL_){
        IAIMPString* s=nullptr;
        eCacheWL_->GetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, IID_IAIMPString, (void**)&s);
        if(s){ cfg.cacheWhitelist = AimpStringToWString(s); s->Release(); }
    }
    if(cboLyric_){
        int idx = 1;
        cboLyric_->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &idx);
        cfg.lyricMode = (idx == 0) ? L"none" : L"uslt";
    }
    // 收集歌单树当前勾选状态(此前勾选从未被保存, "应用"也不会点亮)
    CollectSelection(cfg);
    ConfigManager::Save(cfg);
    // 换 Cookie 视为重新登录: 刷新设备指纹(仅登录时刷新, 平时复用)
    if(!cfg.cookie.empty() && cfg.cookie != oldCookie)
        NcmClient::RegenerateDeviceCookie();
    if(notify && optSvc_) optSvc_->FrameModified(this);
}

// ---------- 操作 ----------

void NcmOptionsFrame::RefreshPlaylists(){
    if(!form_) return;
    SaveConfig(false);
    ShowStatus(L"正在拉取歌单...");

    ProgCtx* ctx = new ProgCtx();
    ctx->done     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ctx->progress = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    TaskCenter::Run([ctx]{
        try{
        NcmConfig cfg; ConfigManager::Load(cfg);
        NcmClient client(cfg);
        long long uid=0;
        if(!cfg.uid.empty()) uid=_wtoi64(cfg.uid.c_str());
        if(uid==0 && client.GetAccountId(uid)){
            cfg.uid = std::to_wstring(uid);
            ConfigManager::Save(cfg);
        }
        if(uid==0){
            ctx->resultCode = 1;
            ctx->resultMsg = cfg.cookie.empty()
                ? L"未登录 · 请先粘贴 Cookie"
                : L"已填入 Cookie 但获取 UID 失败 · 请检查网络/镜像可用性，确认 MUSIC_U 有效后重试";
            SetEvent(ctx->done); return;
        }
        if(cfg.uid.empty()){ cfg.uid = std::to_wstring(uid); ConfigManager::Save(cfg); }
        if(client.GetUserPlaylists(uid, ctx->playlists, 200)){
            ctx->resultCode = 0;
        } else {
            ctx->resultCode = 1;
            ctx->resultMsg = L"获取歌单失败 · 请检查网络/代理/登录状态";
        }
        }catch(...){
            ctx->resultCode = 1; ctx->resultMsg = L"拉取歌单线程异常";
            FsLog("RefreshPlaylists worker exception");
        }
        SetEvent(ctx->done);
    });

    RunProgressDialog(this, core_, ctx);
    if(ctx->cancel.load()){
        ShowStatus(L"已取消拉取歌单");
    } else if(ctx->resultCode == 0){
        ApplyPlaylists(ctx->playlists);
    } else {
        ShowStatus(ctx->resultMsg);
    }
    CloseHandle(ctx->progress); CloseHandle(ctx->done);
    delete ctx;
}

void NcmOptionsFrame::TestConnection(){
    if(!form_) return;
    SaveConfig(false);
    NcmConfig cfg; ConfigManager::Load(cfg);
    bool useProxy = cfg.useProxy;
    std::wstring api = NormalizeApiUrl(cfg.apiUrl);
    if(useProxy && api.empty()) api = L"http://localhost:3000";
    ShowStatus(L"正在测试连接...");

    ProgCtx* ctx = new ProgCtx();
    ctx->done     = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ctx->progress = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    TaskCenter::Run([ctx, api, useProxy]{
        try{
        if(!useProxy){
            ctx->resultCode = 0;
            ctx->resultMsg = L"直连模式：无需测试（将直连 music.163.com，使用 weapi/eapi）";
        } else {
            std::wstring url = api;
            if(!url.empty() && url.back()==L'/') url.pop_back();
            url += L"/login/qr/key?timestamp=" + std::to_wstring(GetTickCount());
            auto r = HttpClient::Get(url);
            if(r.status==200 && r.body.find("unikey")!=std::string::npos){
                ctx->resultCode = 0;
                ctx->resultMsg = L"连接成功 · 镜像可用 (" + api + L")";
            } else if(r.status!=0){
                char b[64]; sprintf_s(b,"HTTP %d", r.status);
                ctx->resultCode = 1;
                ctx->resultMsg = L"连接失败 · " + Utf8ToWide(b) + L"  请检查镜像是否启动 (npm start)";
            } else {
                ctx->resultCode = 1;
                ctx->resultMsg = L"连接失败 · 无法访问 " + api + L"  请检查地址/防火墙";
            }
        }
        }catch(...){
            ctx->resultCode = 1; ctx->resultMsg = L"测试连接线程异常";
            FsLog("TestConnection worker exception");
        }
        SetEvent(ctx->done);
    });

    RunProgressDialog(this, core_, ctx);
    if(ctx->cancel.load()){
        ShowStatus(L"已取消测试");
    } else {
        ShowStatus(ctx->resultMsg);
    }
    CloseHandle(ctx->progress); CloseHandle(ctx->done);
    delete ctx;
}

HWND NcmOptionsFrame::GetHandle(){
    if(!form_) return nullptr;
    return ((IAIMPUIWinControl*)form_)->GetHandle();
}

// ---------- 消息处理（PostMessage 回主线程更新） ----------

LRESULT CALLBACK NcmOptionsFrame::FrameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam){
    NcmOptionsFrame* self=(NcmOptionsFrame*)GetWindowLongPtrW(hWnd,GWLP_USERDATA);
    WNDPROC prev = self ? self->notifPrevProc_ : nullptr;
    switch(msg){
    case WM_USER+2:{
        std::wstring* p=(std::wstring*)lParam;
        if(p){
            if(self->st_ && IsWindow(hWnd)) self->st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(p->c_str()));
            delete p;
        }
        return 0;
    }
    case WM_USER+3:{
        struct Data{ NcmOptionsFrame* f; std::vector<NcmPlaylist> p; };
        Data* d=(Data*)lParam;
        if(d && self->lst_ && IsWindow(hWnd)){
            self->loading_ = true;  // 程序化填充期间屏蔽 OnTreeChanged
            // 清空现有节点
            self->lst_->Clear();
            IAIMPUITreeListNode* root=nullptr;
            if(SUCCEEDED(self->lst_->GetRootNode(IID_IAIMPUITreeListNode, (void**)&root)) && root){
                NcmConfig cfgSel; ConfigManager::Load(cfgSel);  // 只读一次, 别在节点循环里反复读盘
                // 通过 SetPath 添加顶层节点
                for(auto& pl : d->p){
                    std::wstring txt = pl.name + L"  ·  " + std::to_wstring(pl.trackCount) + L"首";
                    if(!pl.creator.empty()) txt += L"  ·  " + pl.creator;
                    // 节点文本用 IAIMPString
                    IAIMPString* s = self->MakeStr(txt.c_str());
                    if(s){
                        IAIMPUITreeListNode* node=nullptr;
                        // 添加子节点到根
                        if(SUCCEEDED(root->Add(&node)) && node){
                            node->SetValue(0, s); // 列 0 文本
                            node->SetValueAsInt64(AIMPUI_TL_NODE_PROPID_TAG, pl.id);
                            bool sel = std::find(cfgSel.selectedPlaylists.begin(), cfgSel.selectedPlaylists.end(), pl.id)!=cfgSel.selectedPlaylists.end();
                            node->SetValueAsInt32(AIMPUI_TL_NODE_PROPID_CHECKED, sel?AIMPUI_CHECKSTATE_CHECKED:AIMPUI_CHECKSTATE_UNCHECKED);
                            node->Release();
                        }
                        s->Release();
                    }
                }
                root->Release();
            }
            wchar_t buf[64];
            swprintf_s(buf, L"共 %d 个歌单", (int)d->p.size());
            if(self->lblCnt_) self->lblCnt_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(buf));
            if(self->st_) self->st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(L"歌单加载完成，勾选后点“应用”保存"));
            self->SaveConfig(false);
            self->loading_ = false;
            // 上一步 SaveConfig 在 loading_=true 时被跳过, 这里补一次落盘恢复的勾选状态
            self->SaveConfig(false);
        }
        if(d) delete d;
        return 0;
    }
    case WM_USER+5:{
        // 同步完成: 主线程导入 m3u8 到播放列表「网易云串流」(复用同名, 避免重复新建)
        std::wstring* p=(std::wstring*)lParam;
        int total=(int)wParam;
        if(p && self->core_ && IsWindow(hWnd)){
            if(self->st_) self->st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(L"正在导入播放列表..."));
            // 服务对象必须用 QueryInterface 获取(CreateObject 拿不到服务, 会静默失败)
            IAIMPServicePlaylistManager* pm=nullptr;
            if( SUCCEEDED(self->core_->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&pm))
             || SUCCEEDED(self->core_->CreateObject(IID_IAIMPServicePlaylistManager, (void**)&pm)) ){
                if(pm){
                    IAIMPString* name=self->MakeStr(L"网易云串流");
                    IAIMPPlaylist* pl=nullptr;
                    if(name){
                        if(FAILED(pm->GetLoadedPlaylistByName(name, &pl)) || !pl)
                            pm->CreatePlaylist(name, TRUE, &pl);
                    }
                    if(pl){
                        IAIMPString* path=self->MakeStr(p->c_str());
                        HRESULT hrAdd=E_FAIL;
                        if(path){
                            pl->BeginUpdate();
                            pl->DeleteAll();
                            hrAdd = pl->Add(path, 0, -1);
                            pl->EndUpdate();
                            path->Release();
                        }
                        wchar_t b[128];
                        if(SUCCEEDED(hrAdd)) swprintf_s(b, L"已同步 %d 首到播放列表「网易云串流」(本地代理, 播放时实时取链)", total);
                        else swprintf_s(b, L"同步失败 (Add 0x%08X)", (unsigned)hrAdd);
                        if(self->st_) self->st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(b));
                    } else {
                        if(self->st_) self->st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(L"无法创建/找到播放列表"));
                    }
                    if(name) name->Release();
                    if(pl) pl->Release();
                }
            } else {
                if(self->st_) self->st_->SetValueAsObject(AIMPUI_LABEL_PROPID_TEXT, self->MakeStr(L"获取 PlaylistManager 服务失败"));
            }
            if(pm) pm->Release();
        }
        delete p;
        return 0;
    }
    case WM_NCDESTROY:
        // 还原窗口过程(自有消息窗口销毁时)
        if(self){
            if(prev) SetWindowLongPtrW(hWnd, GWLP_WNDPROC, (LONG_PTR)prev);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            self->notifPrevProc_ = nullptr;
            prev = nullptr;
        }
        break;
    }
    return prev ? CallWindowProcW(prev,hWnd,msg,wParam,lParam) : DefWindowProcW(hWnd,msg,wParam,lParam);
}
