#pragma once
#include <string>
#include <windows.h>
#include "../third_party/aimp_sdk/apiCore.h"

// 访问受限(403/429 等)警告: 全插件统一入口, 任意网络层发现访问异常时调用,
// 冷却期内不重复弹窗, 弹窗调度到 AIMP 主线程执行。
void NcmErrorNotifyInit(IAIMPCore* core);
void NcmErrorNotifyAccess(int httpStatus);
// 拉流多次重试均失败时, 提示用户该曲不可用(每首带冷却, 防连续失败刷屏)
// details: 逐次失败记录(代码/报错内容), 会展示在弹窗里
void NcmErrorNotifyTrackUnavailable(long long tid, const std::wstring& title, const std::wstring& details = L"");
