// winsock2 必须先于 windows.h(config.h 间接引入) 包含, 否则 winsock 重定义
#include <winsock2.h>
#include <ws2tcpip.h>
#include "local_server.h"
#include "config.h"
#include "ncm_client.h"
#include "utils.h"
#include <atomic>
#include <thread>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {
    std::atomic<bool> g_run{false};
    SOCKET g_listen = INVALID_SOCKET;
    std::thread g_acceptThread;

    bool ParsePath(const std::string& path, long long& pid, long long& tid){
        // 形如 "/781962518/1360740756.mp3"
        if(path.size() < 2 || path[0] != '/') return false;
        size_t slash = path.find('/', 1);
        if(slash == std::string::npos) return false;
        std::string ps = path.substr(1, slash - 1);
        std::string ts = path.substr(slash + 1);
        size_t dot = ts.find('.');
        if(dot != std::string::npos) ts = ts.substr(0, dot);
        try{
            pid = std::stoll(ps);
            tid = std::stoll(ts);
            return pid > 0 && tid > 0;
        }catch(...){ return false; }
    }

    void SendAll(SOCKET s, const std::string& data){
        int sent = 0, total = (int)data.size();
        while(sent < total){
            int w = send(s, data.c_str() + sent, total - sent, 0);
            if(w <= 0) return;
            sent += w;
        }
    }

    void HandleClient(SOCKET s){
        char buf[4096] = {0};
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if(n <= 0){ closesocket(s); return; }
        buf[n] = 0;

        std::string req(buf);
        std::string path;
        if(req.rfind("GET ", 0) == 0){
            size_t sp = req.find(' ', 4);
            if(sp == std::string::npos) sp = req.find('\r', 4);
            if(sp != std::string::npos) path = req.substr(4, sp - 4);
        }

        std::string response;
        long long pid = 0, tid = 0;
        if(ParsePath(path, pid, tid)){
            NcmConfig cfg; ConfigManager::Load(cfg);
            NcmClient client(cfg);
            std::string url, type;
            if(client.GetSongUrl(tid, url, type) && !url.empty()){
                response = "HTTP/1.1 302 Found\r\nLocation: " + WideToUtf8(Utf8ToWide(url)) +
                           "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
        }
        if(response.empty())
            response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

        SendAll(s, response);
        shutdown(s, SD_SEND);
        closesocket(s);
    }

    void AcceptLoop(){
        while(g_run){
            SOCKET c = accept(g_listen, nullptr, nullptr);
            if(c == INVALID_SOCKET) break;
            // 每连接一线程, 取链耗时不阻塞后续请求(AIMP 可能预取下一首)
            std::thread([c]{ HandleClient(c); }).detach();
        }
    }
}

bool LocalServer::Start(int preferredPort, int* boundPort){
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2), &wsa) != 0) return false;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(g_listen == INVALID_SOCKET){ WSACleanup(); return false; }

    BOOL reuse = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 仅监听本机回环

    int port = preferredPort > 0 ? preferredPort : 47777;
    bool ok = false;
    for(int i = 0; i < 20; ++i){
        addr.sin_port = htons((u_short)(port + i));
        if(bind(g_listen, (sockaddr*)&addr, sizeof(addr)) == 0){
            ok = true; port = port + i; break;
        }
    }
    if(!ok || listen(g_listen, 8) != 0){
        closesocket(g_listen); g_listen = INVALID_SOCKET; WSACleanup();
        return false;
    }

    g_run = true;
    g_acceptThread = std::thread(AcceptLoop);
    if(boundPort) *boundPort = port;
    return true;
}

void LocalServer::Stop(){
    g_run = false;
    if(g_listen != INVALID_SOCKET){
        closesocket(g_listen);      // 使阻塞中的 accept 返回并退出线程
        g_listen = INVALID_SOCKET;
    }
    if(g_acceptThread.joinable()) g_acceptThread.join();
    WSACleanup();
}
