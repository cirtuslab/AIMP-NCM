#include "task_center.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

extern void FsLog(const char* what);   // 定义于 filesystem.cpp

namespace {
std::mutex g_taskMtx;
std::vector<std::thread> g_tasks;
std::atomic<bool> g_shutdown{false};
}

namespace TaskCenter {

void Run(std::function<void()> fn){
    std::lock_guard<std::mutex> lk(g_taskMtx);
    if(g_shutdown.load()) return;   // 已进入退出流程, 不再启动新任务
    g_tasks.emplace_back([fn]{
        try{ fn(); }catch(...){ FsLog("TaskCenter: task exception"); }
    });
}

void Shutdown(){
    g_shutdown.store(true);
    std::vector<std::thread> tasks;
    {
        std::lock_guard<std::mutex> lk(g_taskMtx);
        tasks.swap(g_tasks);
    }
    for(auto& t : tasks)
        if(t.joinable()) t.join();
}

bool IsShuttingDown(){
    return g_shutdown.load();
}

} // namespace TaskCenter
