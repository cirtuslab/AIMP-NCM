// SPDX-License-Identifier: MIT
#pragma once

// 内嵌本地 HTTP 服务: 接收 http://127.0.0.1:{port}/{pid}/{tid} 请求,
// 实时解析真实 CDN 链接并直接代理拉流(非 302), 流内注入 ID3v2 标签。
// 播放列表直接使用 http:// 条目, 由 AIMP 原生支持, 彻底规避自定义协议路由问题。
namespace LocalServer {
    // 启动服务; preferredPort 被占用时自动向后尝试; 成功时经 boundPort 返回实际端口。
    // 若端口已被另一个可用的同插件实例占用, 则复用它(此时 IsExternal()==true)。
    bool Start(int preferredPort, int* boundPort);
    // 当前是否复用其他 AIMP 实例的本地服务(本实例未监听)
    bool IsExternal();
    void Stop();
    // 立即按当前配置执行一次缓存清理(设置保存后调用)
    void RunCleanupNow();
}
