#include "ncm_login_form.h"
#include "config.h"
#include "ncm_client.h"
#include "utils.h"
#include "http_client.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <shellapi.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

static std::string g_qrKey;
static IAIMPCore* g_core = nullptr;

void NcmLoginForm::Show(HWND parent, IAIMPCore* core){
    g_core = core;
    // 使用简单 Win32 对话框，资源 ID 102 (需在 plugin.rc 中定义)
    DialogBoxParamW(GetModuleHandleW(L"aimp_ncm.dll"), MAKEINTRESOURCEW(102), parent, DlgProc, 0);
}

INT_PTR CALLBACK NcmLoginForm::DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, 2001, L"点击获取二维码");
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wParam)){
        case 2002: // 获取二维码
            StartQrPoll(hDlg);
            break;
        case IDCANCEL:
            EndDialog(hDlg, 0);
            break;
        }
        break;
    case WM_USER+1:
        if(wParam==803){
            SetDlgItemTextW(hDlg, 2001, L"登录成功!");
            EndDialog(hDlg, 1);
        } else if(wParam==800){
            SetDlgItemTextW(hDlg, 2001, L"二维码过期");
        }
        break;
    case WM_USER+2: {
        std::wstring* p=(std::wstring*)lParam;
        if(p){ SetDlgItemTextW(hDlg, 2001, p->c_str()); delete p; }
        break;
    }
    case WM_USER+3: {
        // 加载 PNG 并显示在 2003（用 WIC，OleLoadPicture 不支持 PNG 会失败 0x800A01E1）
        std::wstring* pPath=(std::wstring*)lParam;
        if(pPath){
            HWND pic = GetDlgItem(hDlg, 2003);
            if(pic){
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                IWICImagingFactory* factory=nullptr;
                if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))){
                    IWICBitmapDecoder* decoder=nullptr;
                    if(SUCCEEDED(factory->CreateDecoderFromFilename(pPath->c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))){
                        IWICBitmapFrameDecode* frame=nullptr;
                        if(SUCCEEDED(decoder->GetFrame(0, &frame))){
                            UINT w=0, h=0;
                            frame->GetSize(&w, &h);
                            IWICFormatConverter* conv=nullptr;
                            if(SUCCEEDED(factory->CreateFormatConverter(&conv))){
                                if(SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))){
                                    std::vector<BYTE> buf((size_t)w*h*4);
                                    if(SUCCEEDED(conv->CopyPixels(nullptr, w*4, (UINT)buf.size(), buf.data()))){
                                        BITMAPINFO bmi = {};
                                        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                                        bmi.bmiHeader.biWidth = (LONG)w;
                                        bmi.bmiHeader.biHeight = -(LONG)h;
                                        bmi.bmiHeader.biPlanes = 1;
                                        bmi.bmiHeader.biBitCount = 32;
                                        bmi.bmiHeader.biCompression = BI_RGB;
                                        HDC hdc = GetDC(nullptr);
                                        HBITMAP hBmp = CreateCompatibleBitmap(hdc, w, h);
                                        HDC memdc = CreateCompatibleDC(hdc);
                                        HGDIOBJ old = SelectObject(memdc, hBmp);
                                        SetDIBits(memdc, hBmp, 0, h, buf.data(), &bmi, DIB_RGB_COLORS);
                                        SelectObject(memdc, old);
                                        DeleteDC(memdc);
                                        ReleaseDC(nullptr, hdc);
                                        // 替换旧图（若有）
                                        HBITMAP oldBmp=(HBITMAP)SendMessageW(pic, STM_GETIMAGE, IMAGE_BITMAP, 0);
                                        SendMessageW(pic, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)hBmp);
                                        if(oldBmp) DeleteObject(oldBmp);
                                    }
                                }
                                conv->Release();
                            }
                            frame->Release();
                        }
                        decoder->Release();
                    }
                    factory->Release();
                }
                CoUninitialize();
            }
            delete pPath;
        }
        break;
    }
    case WM_CLOSE:
        EndDialog(hDlg,0);
        break;
    }
    return FALSE;
}

void NcmLoginForm::StartQrPoll(HWND hDlg){
    SetDlgItemTextW(hDlg, 2001, L"正在获取二维码...");
    // 清空旧图
    HWND pic = GetDlgItem(hDlg, 2003);
    if(pic){
        HBITMAP old=(HBITMAP)SendMessageW(pic, STM_GETIMAGE, IMAGE_BITMAP, 0);
        SendMessageW(pic, STM_SETIMAGE, IMAGE_BITMAP, 0);
        if(old) DeleteObject(old);
    }
    std::thread([hDlg]{
        NcmConfig cfg; ConfigManager::Load(cfg);
        NcmClient client(cfg);
        QrLogin qr;
        if(!client.QrCreate(qr)){
            PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)new std::wstring(L"获取二维码失败，请检查代理/网络"));
            return;
        }
        g_qrKey = qr.unikey;
        // 下载二维码图片：多个源依次尝试（qrserver 海外，pwmqr 国内可达）
        std::wstring wenc = Utf8ToWide(HttpClient::UrlEncodeW(qr.qrUrl));
        const std::wstring sources[] = {
            L"https://api.qrserver.com/v1/create-qr-code/?size=200x200&data=" + wenc,
            L"https://api.pwmqr.com/qrcode/create/?url=" + wenc,
            L"http://api.pwmqr.com/qrcode/create/?url=" + wenc,
        };
        for(auto& src : sources){
            auto imgResp = HttpClient::Get(src);
            // 校验 PNG 魔数 (89 50 4E 47)，避免拿到错误页
            if(imgResp.status==200 && imgResp.body.size()>8 &&
               (unsigned char)imgResp.body[0]==0x89 && (unsigned char)imgResp.body[1]==0x50 &&
               (unsigned char)imgResp.body[2]==0x4E && (unsigned char)imgResp.body[3]==0x47){
                wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
                std::wstring pngPath = std::wstring(tmp) + L"ncm_qr.png";
                HANDLE hFile = CreateFileW(pngPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if(hFile!=INVALID_HANDLE_VALUE){
                    DWORD w=0;
                    WriteFile(hFile, imgResp.body.data(), (DWORD)imgResp.body.size(), &w, nullptr);
                    CloseHandle(hFile);
                    std::wstring* pPath = new std::wstring(pngPath);
                    PostMessageW(hDlg, WM_USER+3, 0, (LPARAM)pPath);
                }
                break; // 成功获取即跳出
            }
        }
        std::wstring txt = L"请用网易云APP扫码\n" + qr.qrUrl;
        PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)new std::wstring(txt));
        std::atomic<bool> challengeOpened{false};
        for(int i=0;i<60;i++){
            Sleep(2000);
            if(!IsWindow(hDlg)) return;
            std::string cookie; std::wstring cmsg; std::wstring chal; long long uid=0;
            int code = client.QrCheck(g_qrKey, cookie, cmsg, &chal, &uid);
            if(code==803){
                NcmConfig c2; ConfigManager::Load(c2);
                c2.cookie = Utf8ToWide(cookie);
                // uid 优先取自 803 响应体；MUSIC_U 现已是随机 token，base64 解析仅作兜底
                if(uid<=0) uid = NcmClient::ParseUidFromCookie(c2.cookie);
                if(uid>0) c2.uid = std::to_wstring(uid);
                ConfigManager::Save(c2);
                PostMessageW(hDlg, WM_USER+1, 803, 0);
                return;
            } else if(code==800){
                PostMessageW(hDlg, WM_USER+1, 800, 0);
                return;
            } else if(!chal.empty()){
                // 风控滑块 challenge: 用默认浏览器弹出验证页(每次登录仅弹一次)，
                // 不中止轮询——用户完成验证后同一 unikey 通常会直接放行(803)
                if(!challengeOpened.exchange(true)){
                    ShellExecuteW(nullptr, L"open", chal.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    std::wstring* p=new std::wstring(L"检测到风控验证，已在浏览器打开滑块验证页，完成后自动继续登录");
                    PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)p);
                } else {
                    PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)new std::wstring(L"等待验证完成… 请在浏览器完成滑块验证"));
                }
            } else {
                std::wstring* p=new std::wstring(L"状态: "+cmsg+L" ("+std::to_wstring(code)+L")");
                PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)p);
            }
        }
        PostMessageW(hDlg, WM_USER+2, 0, (LPARAM)new std::wstring(L"轮询超时"));
    }).detach();
}
