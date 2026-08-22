#pragma once

// 内嵌本地 HTTP 服务: 接收 http://127.0.0.1:{port}/{pid}/{tid}.mp3 请求,
// 实时解析真实播放链接后以 302 重定向返回。
// 播放列表直接使用 http:// 条目, 由 AIMP 原生支持, 彻底规避自定义协议路由问题。
namespace LocalServer {
    // 启动服务; preferredPort 被占用时自动向后尝试; 成功时经 boundPort 返回实际端口
    bool Start(int preferredPort, int* boundPort);
    void Stop();
    // 立即按当前配置执行一次缓存清理(设置保存后调用)
    void RunCleanupNow();
}
