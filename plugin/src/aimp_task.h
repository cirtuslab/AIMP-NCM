#pragma once
#include "../third_party/aimp_sdk/apiThreading.h"
#include <functional>

// 简易 IAIMPTask 封装，替代 std::thread，使用 AIMP 线程池
class AimpTask : public IAIMPTask, public IAIMPTaskPriority {
public:
    using Func = std::function<void(IAIMPTaskOwner*)>;
    explicit AimpTask(Func f, int prio = 0): func_(f), prio_(prio) {}
    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override {
        if(!ppv) return E_POINTER;
        *ppv=nullptr;
        if(IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IAIMPTask)) *ppv=static_cast<IAIMPTask*>(this);
        else if(IsEqualIID(riid, IID_IAIMPTaskPriority)) *ppv=static_cast<IAIMPTaskPriority*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG WINAPI Release() override { ULONG c=InterlockedDecrement(&ref_); if(c==0) delete this; return c; }
    // IAIMPTask
    void WINAPI Execute(IAIMPTaskOwner* Owner) override { if(func_) func_(Owner); }
    // IAIMPTaskPriority
    int WINAPI GetPriority() override { return prio_; }
private:
    volatile LONG ref_=1;
    Func func_;
    int prio_;
};

// 辅助：在主线程执行 lambda
inline void ExecuteInMainThread(IAIMPCore* core, std::function<void()> f){
    IAIMPServiceThreads* svc=nullptr;
    if(SUCCEEDED(core->CreateObject(IID_IAIMPServiceThreads, (void**)&svc)) || SUCCEEDED(core->QueryInterface(IID_IAIMPServiceThreads, (void**)&svc))){
        AimpTask* task = new AimpTask([f](IAIMPTaskOwner*){ f(); });
        svc->ExecuteInMainThread(task, 0);
        task->Release();
        svc->Release();
    } else {
        // 回退 PostMessage
        f();
    }
}
