#include "console.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "common.h"

namespace console {
namespace {

std::mutex queue_lock;
std::vector<std::unique_ptr<Request>> queue;

void serve(SOCKET c) {
    std::string buf;
    char tmp[1024];
    for (;;) {
        int n = recv(c, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto req = std::make_unique<Request>();
            req->line = line;
            auto fut = req->reply.get_future();
            {
                std::lock_guard lk(queue_lock);
                queue.push_back(std::move(req));
            }
            std::string reply = fut.wait_for(std::chrono::seconds(120)) == std::future_status::ready
                                    ? fut.get()
                                    : "error: timed out";
            reply += "\n";
            send(c, reply.data(), (int)reply.size(), 0);
        }
    }
    closesocket(c);
}

}  // namespace

bool start(int port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // local only
    if (bind(s, (sockaddr*)&addr, sizeof addr) != 0 || listen(s, 4) != 0) {
        LOGE("console: cannot listen on 127.0.0.1:%d", port);
        closesocket(s);
        return false;
    }
    LOGI("console listening on 127.0.0.1:%d", port);
    std::thread([s] {
        for (;;) {
            SOCKET c = accept(s, nullptr, nullptr);
            if (c == INVALID_SOCKET) break;
            std::thread(serve, c).detach();
        }
    }).detach();
    return true;
}

std::vector<std::unique_ptr<Request>> take() {
    std::lock_guard lk(queue_lock);
    return std::move(queue);
}

}  // namespace console
