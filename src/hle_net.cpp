// Networking. Gameloft's online services are gone, so for now every socket
// call fails cleanly with ENETDOWN. LAN play can be built on this later
// (Linux and Winsock socket option constants differ and need translating).
#include "hle.h"

namespace hle {
namespace {

constexpr int ENETDOWN_ = 100;

s32 fail_net() {
    set_errno(ENETDOWN_);
    return -1;
}
s32 h_socket(int, int, int) { return fail_net(); }
s32 h_fail2(int, u32) { return fail_net(); }
s32 h_fail3(int, u32, u32) { return fail_net(); }
s32 h_fail4(int, u32, u32, u32) { return fail_net(); }
s32 h_fail5(int, u32, u32, u32, u32) { return fail_net(); }
s32 h_fail6(int, u32, u32, u32, u32, u32) { return fail_net(); }
s32 h_select(int, u32, u32, u32, u32) { return 0; }
s32 h_ioctl(int, u32, u32) { return fail_net(); }
s32 h_fcntl(int, int, u32) { return 0; }
u32 h_gethostbyname(const char* name) {
    LOGI("gethostbyname(%s) -> offline", name);
    return 0;
}
int h_getaddrinfo(const char* node, const char*, u32, u32* res) {
    LOGI("getaddrinfo(%s) -> offline", node ? node : "");
    if (res) *res = 0;
    return 4;  // EAI_FAIL
}
void h_freeaddrinfo(u32) {}
int h_gethostname(char* buf, u32 len) {
    strncpy(buf, "localhost", len);
    return 0;
}
u32 h_inet_addr(const char* s) {
    u32 a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xFFFFFFFF;
    return a | (b << 8) | (c << 16) | (d << 24);
}
u32 inet_buf;
u32 h_inet_ntoa(u32 addr) {
    snprintf((char*)mem::H(inet_buf), 16, "%u.%u.%u.%u", addr & 255, (addr >> 8) & 255, (addr >> 16) & 255, addr >> 24);
    return inet_buf;
}

}  // namespace

void init_net() {
    inet_buf = mem::data_alloc(16);
#define R(name, fn) reg(name, W<fn>)
    R("socket", h_socket); R("listen", h_fail2); R("accept", h_fail3); R("bind", h_fail3);
    R("connect", h_fail3); R("getsockname", h_fail3); R("recv", h_fail4); R("send", h_fail4);
    R("recvfrom", h_fail6); R("sendto", h_fail6); R("setsockopt", h_fail5); R("getsockopt", h_fail5);
    R("select", h_select); R("ioctl", h_ioctl); R("fcntl", h_fcntl);
    R("gethostbyname", h_gethostbyname); R("getaddrinfo", h_getaddrinfo); R("freeaddrinfo", h_freeaddrinfo);
    R("gethostname", h_gethostname); R("inet_addr", h_inet_addr); R("inet_ntoa", h_inet_ntoa);
#undef R
}

}  // namespace hle
