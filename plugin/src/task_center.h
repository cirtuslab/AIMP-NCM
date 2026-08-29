#pragma once
#include <functional>
#include <windows.h>

// B1 修复: 后台任务由插件统一跟踪。
//  - Run: 注册一个后台线程(内部捕获异常), Finalize 调用 Shutdown 时全部 join;
//  - 后台任务不得捕获已销毁的 UI 对象, 只通过进度对话框/堆对象通信。
namespace TaskCenter {
    void Run(std::function<void()> fn);
    void Shutdown();          // 置取消标志并等待所有任务结束
    bool IsShuttingDown();
}
