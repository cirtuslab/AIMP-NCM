#include <windows.h>
#include "aimp_ncm_plugin.h"

HINSTANCE g_hInst=nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) g_hInst=hModule;
    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** Header) {
    if(!Header) return E_POINTER;
    *Header = new AimpNcmPlugin();
    return S_OK;
}
