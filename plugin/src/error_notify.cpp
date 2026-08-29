#include "error_notify.h"
#include "aimp_task.h"
#include "../third_party/aimp_sdk/apiGUI.h"
#include "../third_party/aimp_sdk/apiObjects.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace {

std::atomic<IAIMPCore*> g_core{nullptr};
std::atomic<ULONGLONG> g_lastNotifyMs{0};
const ULONGLONG kCooldownMs = 60000;   // 60s 内同源警告只弹一次, 防刷屏

std::mutex g_trackMtx;
std::map<long long, ULONGLONG> g_trackLastNotify;
const ULONGLONG kTrackCooldownMs = 30000;   // 同一首歌 30s 内只提示一次

std::wstring Describe(int code){
    switch(code){
    case 401: return L"401 Unauthorized（未授权，Cookie 可能已失效）";
    case 402: return L"402 Payment Required（付费内容无权访问）";
    case 403: return L"403 Forbidden（拒绝访问/风控）";
    case 429: return L"429 Too Many Requests（请求过于频繁/被限流）";
    case 451: return L"451 Unavailable For Legal Reasons";
    default:  return std::to_wstring(code) + L"（访问受限）";
    }
}

// AIMP 皮肤消息框(必须主线程调用)
void ShowAimpMessage(IAIMPCore* core, const std::wstring& caption, const std::wstring& text){
    IAIMPServiceUI* uiSvc = nullptr;
    if(FAILED(core->QueryInterface(IID_IAIMPServiceUI, (void**)&uiSvc)) || !uiSvc) return;
    IAIMPUIMessageDialog* dlg = nullptr;
    if(SUCCEEDED(uiSvc->QueryInterface(IID_IAIMPUIMessageDialog, (void**)&dlg)) && dlg){
        IAIMPString* cap = nullptr;
        IAIMPString* txt = nullptr;
        core->CreateObject(IID_IAIMPString, (void**)&cap);
        core->CreateObject(IID_IAIMPString, (void**)&txt);
        if(cap) cap->SetData((TChar*)caption.c_str(), (int)caption.size());
        if(txt) txt->SetData((TChar*)text.c_str(), (int)text.size());
        dlg->Execute(0, cap, txt, MB_OK | MB_ICONWARNING);
        if(cap) cap->Release();
        if(txt) txt->Release();
        dlg->Release();
    }
    uiSvc->Release();
}

} // namespace

void NcmErrorNotifyInit(IAIMPCore* core){
    g_core.store(core);
}

void NcmErrorNotifyAccess(int httpStatus){
    if(httpStatus != 401 && httpStatus != 402 && httpStatus != 403 &&
       httpStatus != 429 && httpStatus != 451) return;
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastNotifyMs.load();
    if(now - last < kCooldownMs) return;
    if(!g_lastNotifyMs.compare_exchange_strong(last, now)) return;
    IAIMPCore* core = g_core.load();
    if(!core) return;
    std::wstring msg =
        L"AIMP NCM 检测到访问受限：HTTP " + Describe(httpStatus) +
        L"\n\n可能原因：网易云风控/限流、Cookie 失效、或镜像服务不可用。\n"
        L"建议：稍后重试；检查 MUSIC_U Cookie 是否有效；或更换/关闭镜像代理。";
    ExecuteInMainThread(core, [core, msg]{
        ShowAimpMessage(core, L"AIMP NCM · 访问受限警告", msg);
    });
}

void NcmErrorNotifyTrackUnavailable(long long tid, const std::wstring& title, const std::wstring& details){
    if(tid <= 0) return;
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(g_trackMtx);
        auto it = g_trackLastNotify.find(tid);
        if(it != g_trackLastNotify.end() && now - it->second < kTrackCooldownMs) return;
        g_trackLastNotify[tid] = now;
        if(g_trackLastNotify.size() > 256){          // 防止缓存无限增长
            g_trackLastNotify.clear();
            g_trackLastNotify[tid] = now;
        }
    }
    IAIMPCore* core = g_core.load();
    if(!core) return;
    std::wstring msg = L"AIMP NCM：歌曲「" + title + L"」无法播放。\n\n";
    if(!details.empty()) msg += L"失败记录：\n" + details + L"\n";
    msg += L"多次尝试仍无法获取播放流，此曲可能受音质/版权/网络限制。\n"
           L"建议：切换音质、检查网络或镜像后重试。";
    ExecuteInMainThread(core, [core, msg]{
        ShowAimpMessage(core, L"AIMP NCM · 曲目不可用", msg);
    });
}
