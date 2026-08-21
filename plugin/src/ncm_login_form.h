#pragma once
#include "../third_party/aimp_sdk/apiCore.h"
#include <windows.h>

// 模态登录窗：显示二维码 + 轮询
class NcmLoginForm {
public:
    static void Show(HWND parent, IAIMPCore* core);
private:
    static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
    static void StartQrPoll(HWND hDlg);
};
