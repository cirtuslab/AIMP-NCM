#include "ui_dialog.h"
#include "config.h"
#include "ncm_client.h"
#include "utils.h"
#include "http_client.h"
#include "filesystem.h"
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <algorithm>
#include "../third_party/nlohmann/json.hpp"
#include "../third_party/aimp_sdk/apiPlaylists.h"

static NcmConfig g_cfg;
static std::vector<NcmPlaylist> g_playlists;
static std::string g_qrKey;
static HWND g_hDlg=nullptr;

static void RefreshPlaylists(HWND hList){
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for(auto& pl: g_playlists){
        std::wstring txt = pl.name + L" (" + std::to_wstring(pl.trackCount) + L")";
        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)txt.c_str());
        SendMessageW(hList, LB_SETITEMDATA, idx, (LPARAM)pl.id);
        // 选中状态
        bool sel = std::find(g_cfg.selectedPlaylists.begin(), g_cfg.selectedPlaylists.end(), pl.id) != g_cfg.selectedPlaylists.end();
        if(sel) SendMessageW(hList, LB_SETSEL, TRUE, idx);
    }
}
static void OnLoginQr(HWND hDlg){
    // 同步输入框当前内容到 g_cfg, 避免粘贴后未失焦读到旧值
    {
        WCHAR buf[8192]={0};
        GetDlgItemTextW(hDlg, 1001, buf, 1024); g_cfg.apiUrl = buf;
        GetDlgItemTextW(hDlg, 1002, buf, 8192); g_cfg.cookie = NormalizeCookie(buf);
        ConfigManager::Save(g_cfg);
    }
    SetDlgItemTextW(hDlg, 1005, L"正在获取二维码...");
    NcmClient client(g_cfg);
    QrLogin qr;
    if(!client.QrCreate(qr)){
        SetDlgItemTextW(hDlg, 1005, L"获取二维码失败，请检查网络或镜像地址");
        return;
    }
    g_qrKey = qr.unikey;
    SetDlgItemTextW(hDlg, 1005, (L"请用网易云APP扫码: " + qr.qrUrl).c_str());
    // 轮询 (修复: 用 PostMessage 跨线程更新，避免直接 SetDlgItemTextW)
    std::thread([hDlg]{
        NcmClient cl(g_cfg);
        std::atomic<bool> challengeOpened{false};
        for(int i=0;i<60;i++){
            Sleep(2000);
            if(!IsWindow(hDlg)) return;
            std::string cookie; std::wstring msg; std::wstring chal; long long uid=0;
            int code = cl.QrCheck(g_qrKey, cookie, msg, &chal, &uid);
            if(code==803){
                g_cfg.cookie = Utf8ToWide(cookie);
                // 803 响应体直接带 uid；解析不出时兜底
                if(uid<=0) uid = NcmClient::ParseUidFromCookie(g_cfg.cookie);
                if(uid>0) g_cfg.uid = std::to_wstring(uid);
                ConfigManager::Save(g_cfg);
                if(IsWindow(hDlg)) PostMessageW(hDlg, WM_USER+1, 803, 0);
                return;
            } else if(code==800){
                if(IsWindow(hDlg)) PostMessageW(hDlg, WM_USER+1, 800, 0);
                return;
            } else if(!chal.empty()){
                // 风控滑块 challenge: 弹出验证页(每次登录仅一次)，继续轮询
                if(!challengeOpened.exchange(true)){
                    ShellExecuteW(nullptr, L"open", chal.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    std::wstring* p = new std::wstring(L"检测到风控验证，已打开浏览器滑块验证页，完成后自动继续登录");
                    if(IsWindow(hDlg)) PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)p); else delete p;
                } else {
                    std::wstring* p = new std::wstring(L"等待验证完成… 请在浏览器完成滑块验证");
                    if(IsWindow(hDlg)) PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)p); else delete p;
                }
            } else {
                std::wstring* pTxt = new std::wstring(L"状态: " + msg + L" (" + std::to_wstring(code) + L")");
                if(IsWindow(hDlg)) PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)pTxt);
                else delete pTxt;
            }
        }
        if(IsWindow(hDlg)) PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)new std::wstring(L"轮询超时，请重试"));
    }).detach();
}
static void OnRefreshPlaylists(HWND hDlg){
    HWND hList = GetDlgItem(hDlg, 1008);
    // 同步输入框当前内容到 g_cfg, 避免粘贴后未失焦读到旧值
    {
        WCHAR buf[8192]={0};
        GetDlgItemTextW(hDlg, 1001, buf, 1024); g_cfg.apiUrl = buf;
        GetDlgItemTextW(hDlg, 1002, buf, 8192); g_cfg.cookie = NormalizeCookie(buf);
        ConfigManager::Save(g_cfg);
    }
    SetDlgItemTextW(hDlg, 1005, L"正在拉取歌单...");
    // 需要 uid：若 config.uid 为空，尝试通过 /user/account 获取
    NcmConfig cfg=g_cfg;
    NcmClient client(cfg);
    // 先尝试获取用户信息
    std::wstring uidStr = cfg.uid;
    long long uid=0;
    if(!uidStr.empty()) uid = _wtoi64(uidStr.c_str());
    if(uid==0){
        // 双模式补全 uid (镜像/直连)，此前直连模式下无法获取导致拉不到歌单
        if(client.GetAccountId(uid)){
            g_cfg.uid = std::to_wstring(uid);
            ConfigManager::Save(g_cfg);
        }
    }
    if(uid==0){
        if(cfg.cookie.empty()){
            SetDlgItemTextW(hDlg, 1005, L"未登录 · 请先登录（二维码/粘贴Cookie）");
        } else {
            SetDlgItemTextW(hDlg, 1005, L"已填入 Cookie 但获取 UID 失败 · 请检查网络/镜像后重试");
        }
        return;
    }
    std::vector<NcmPlaylist> pls;
    if(client.GetUserPlaylists(uid, pls, 100)){
        g_playlists = pls;
        RefreshPlaylists(hList);
        SetDlgItemTextW(hDlg, 1005, (L"已获取 " + std::to_wstring(pls.size()) + L" 个歌单").c_str());
    } else {
        SetDlgItemTextW(hDlg, 1005, L"获取歌单失败");
    }
}
static std::wstring GetTempM3U8Path(long long pid){
    WCHAR tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring p = std::wstring(tmp) + L"aimp_ncm_" + std::to_wstring(pid) + L".m3u8";
    return p;
}
static void OnSaveAndSync(HWND hDlg, IAIMPCore* core){
    // 保存配置
    WCHAR buf[8192]={0};
    GetDlgItemTextW(hDlg, 1001, buf, 1024); g_cfg.apiUrl = buf;
    GetDlgItemTextW(hDlg, 1002, buf, 8192); g_cfg.cookie = NormalizeCookie(buf);
    // quality combo
    HWND hCombo = GetDlgItem(hDlg, 1007);
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    const wchar_t* levels[] = {L"standard",L"higher",L"exhigh",L"lossless",L"hires",L"jymaster",L"jyeffect",L"sky"};
    if(sel>=0 && sel<8) g_cfg.quality = levels[sel];
    // selected playlists
    HWND hList = GetDlgItem(hDlg, 1008);
    g_cfg.selectedPlaylists.clear();
    int count = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
    for(int i=0;i<count;i++){
        if(SendMessageW(hList, LB_GETSEL, i, 0)){
            long long pid = (long long)SendMessageW(hList, LB_GETITEMDATA, i, 0);
            g_cfg.selectedPlaylists.push_back(pid);
        }
    }
    ConfigManager::Save(g_cfg);
    SetDlgItemTextW(hDlg, 1005, L"配置已保存，正在同步到 AIMP 播放列表...");

    // 同步到 AIMP 播放列表（异步）：后台生成单一合并 m3u8，主线程导入，避免并发加载器崩溃
    IAIMPCore* c = core; NcmConfig cfg = g_cfg;
    std::thread([c,cfg, hDlg]{
        // 后台仅生成 m3u，不直接操作播放列表
        NcmClient client(cfg);
        std::wstring merged = GetTempM3U8Path(0); // 0 表示合并
        // 用 pid 0 的路径会是 aimp_ncm_0.m3u8，作为合并文件
        std::ofstream f(WideToUtf8(merged), std::ios::binary);
        if(!f) {
            PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)new std::wstring(L"无法创建临时文件"));
            return;
        }
        f << "#EXTM3U\n";
        int totalAdded=0, totalPlaylists=0;
        // 批量并发取链：先收集所有歌曲
        struct SongInfo { long long pid; NcmSong song; };
        std::vector<SongInfo> all;
        for(long long pid: cfg.selectedPlaylists){
            std::vector<NcmSong> songs; NcmPlaylist info;
            if(!client.GetPlaylistDetail(pid, songs, &info)) continue;
            if(songs.empty()) continue;
            for(auto& s: songs) all.push_back({pid, s});
            totalPlaylists++;
        }
        // 分批取链 (100/批)
        for(size_t i=0;i<all.size();i+=100){
            size_t end = std::min(all.size(), i+100);
            // 构造批量 id
            std::string ids;
            for(size_t j=i;j<end;++j){
                if(!ids.empty()) ids+=",";
                ids+=std::to_string(all[j].song.id);
            }
            // 逐批取链 (复用 GetSongUrl 的批量能力需扩展 NcmClient，暂逐条但已在后台线程，不阻塞UI)
            // 为避免 1000 次串行阻塞，改为批量接口: 直接调 RequestMirror/RequestDirect 的批量
            // 简化：逐条但已合并为单次导入，显著减少 Popen 次数
            for(size_t j=i;j<end;++j){
                std::string url,type;
                if(client.GetSongUrl(all[j].song.id, url, type) && !url.empty()){
                    std::string extinf = "#EXTINF:" + std::to_string(all[j].song.durationMs/1000) + "," + WideToUtf8(all[j].song.artist) + " - " + WideToUtf8(all[j].song.title) + "\n";
                    f << extinf;
                    f << url << "\n";
                    totalAdded++;
                }
            }
        }
        f.close();
        // 通过 PostMessage 让主线程导入，避免工作线程直接操作播放列表
        std::wstring* pPath = new std::wstring(merged);
        // 传递 total 信息 via 额外消息
        if(IsWindow(hDlg)){
            // 先保存 total 到全局供主线程读取
            static int g_total = 0; g_total = totalAdded;
            PostMessageW(hDlg, WM_USER+3, (WPARAM)totalAdded, (LPARAM)pPath);
        } else delete pPath;
    }).detach();
}

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam){
    static IAIMPCore* s_core=nullptr;
    switch(msg){
    case WM_INITDIALOG:
        s_core = (IAIMPCore*)lParam;
        g_hDlg=hDlg;
        ConfigManager::Load(g_cfg);
        SetDlgItemTextW(hDlg, 1001, g_cfg.apiUrl.c_str());
        SetDlgItemTextW(hDlg, 1002, g_cfg.cookie.c_str());
        SetDlgItemTextW(hDlg, 1003, g_cfg.uid.c_str());
        // quality combo
        {
            HWND hCombo=GetDlgItem(hDlg,1007);
            const wchar_t* names[]={L"标准 (128k)",L"较高 (192k)",L"极高 (320k)",L"无损 (FLAC)",L"Hi-Res",L"高清臻音",L"沉浸环绕",L"超清母带"};
            const wchar_t* levels[]={L"standard",L"higher",L"exhigh",L"lossless",L"hires",L"jymaster",L"jyeffect",L"sky"};
            for(int i=0;i<8;i++) SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)names[i]);
            int sel=2;
            for(int i=0;i<8;i++) if(g_cfg.quality==levels[i]) sel=i;
            SendMessageW(hCombo, CB_SETCURSEL, sel, 0);
        }
        // playlists listbox
        {
            HWND hList=GetDlgItem(hDlg,1008);
            SendMessageW(hList, LB_RESETCONTENT,0,0);
            for(auto pid: g_cfg.selectedPlaylists){
                wchar_t buf[64]; swprintf_s(buf, L"已选: %lld", pid);
                SendMessageW(hList, LB_ADDSTRING,0,(LPARAM)buf);
            }
        }
        SetDlgItemTextW(hDlg, 1005, L"就绪");
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wParam)){
        case 1004: OnLoginQr(hDlg); break;
        case 1006: OnRefreshPlaylists(hDlg); break;
        case IDOK: OnSaveAndSync(hDlg, s_core); break;
        case IDCANCEL: EndDialog(hDlg, 0); break;
        case 1010: ConfigManager::Clear(); SetDlgItemTextW(hDlg, 1005, L"已清除配置"); break;
        }
        break;
    case WM_USER+1:
        if(wParam==803) SetDlgItemTextW(hDlg, 1005, L"扫码成功，已保存 Cookie");
        else if(wParam==800) SetDlgItemTextW(hDlg, 1005, L"二维码过期，请重新获取");
        break;
    case WM_USER+2:
        {
            std::wstring* p = (std::wstring*)lParam;
            if(p){
                if(IsWindow(hDlg)) SetDlgItemTextW(hDlg, 1005, p->c_str());
                delete p;
            }
        }
        break;
    case WM_USER+3:
        {
            std::wstring* pPath = (std::wstring*)lParam;
            int total = (int)wParam;
            if(pPath && IsWindow(hDlg)){
                // 主线程导入，避免工作线程直接操作播放列表导致崩溃
                IAIMPServicePlaylistManager* pm=nullptr;
                if(SUCCEEDED(s_core->CreateObject(IID_IAIMPServicePlaylistManager, (void**)&pm)) || SUCCEEDED(s_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&pm))){
                    IAIMPString* aimpPath=nullptr;
                    s_core->CreateObject(IID_IAIMPString, (void**)&aimpPath);
                    if(aimpPath){
                        aimpPath->SetData((TChar*)pPath->c_str(), (int)pPath->size());
                        IAIMPPlaylist* pl=nullptr;
                        HRESULT hr = pm->CreatePlaylistFromFile(aimpPath, TRUE, &pl);
                        if(pl) pl->Release();
                        aimpPath->Release();
                    }
                    pm->Release();
                }
                SetDlgItemTextW(hDlg, 1005, (L"同步完成，共 " + std::to_wstring(total) + L" 首 (已导入)").c_str());
            }
            delete pPath;
        }
        break;
    case WM_CLOSE: EndDialog(hDlg,0); break;
    }
    return FALSE;
}

void ShowNcmSettingsDialog(HWND parent, IAIMPCore* core){
    DialogBoxParamW(GetModuleHandleW(L"aimp_ncm.dll"), MAKEINTRESOURCEW(101), parent, DlgProc, (LPARAM)core);
    // 若资源未加载，回退到动态创建
    if(GetLastError()==1813){
        // 动态创建简易对话框
        // 为简化，使用 MessageBox 提示用户编辑配置文件
        std::wstring path = ConfigManager::GetConfigPath();
        std::wstring msg = L"配置文件位于:\n" + path + L"\n\n请用文本编辑器修改 cookie / apiUrl / quality 后重启 AIMP。\n\n将自动打开配置文件夹。";
        MessageBoxW(parent, msg.c_str(), L"AIMP NCM 设置", MB_OK);
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
