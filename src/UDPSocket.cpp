// UDPSocket.cpp — Non-blocking UDP socket wrapper (cross-platform)

#include "UDPSocket.h"
#include <XPLMUtilities.h>
#include <cstring>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
   // Winsock initialisation — done once per process
   struct WinsockInit {
       WinsockInit()  { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
       ~WinsockInit() { WSACleanup(); }
   };
   static WinsockInit s_winsock_init;
#  define CLOSE_SOCKET(s) closesocket(s)
#  define SOCK_ERRNO      WSAGetLastError()
#  define EWOULDBLOCK_VAL WSAEWOULDBLOCK
#else
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  define CLOSE_SOCKET(s) ::close(s)
#  define SOCK_ERRNO      errno
#  define EWOULDBLOCK_VAL EWOULDBLOCK
#endif

static const int RECV_BUF_SIZE = 4096;

UDPSocket::UDPSocket() : m_fd(INVALID_SOCKET_VAL) {}
UDPSocket::~UDPSocket() { close(); }

bool UDPSocket::bind(uint16_t port) {
    m_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_fd == INVALID_SOCKET_VAL) {
        XPLMDebugString("[X1000] UDPSocket::bind: socket() failed\n");
        return false;
    }

    // Non-blocking
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(m_fd, FIONBIO, &mode);
#else
    int flags = fcntl(m_fd, F_GETFL, 0);
    fcntl(m_fd, F_SETFL, flags | O_NONBLOCK);
#endif

    // Allow address reuse
    int opt = 1;
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(m_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        XPLMDebugString("[X1000] UDPSocket::bind: bind() failed\n");
        CLOSE_SOCKET(m_fd);
        m_fd = INVALID_SOCKET_VAL;
        return false;
    }
    return true;
}

bool UDPSocket::send(const std::string& msg, const std::string& ip, uint16_t port) {
    if (m_fd == INVALID_SOCKET_VAL) return false;

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) != 1)
        return false;

    int sent = (int)::sendto(m_fd, msg.c_str(), (int)msg.size(), 0,
                             (sockaddr*)&dest, sizeof(dest));
    return sent == (int)msg.size();
}

bool UDPSocket::recv(std::string& out_msg,
                     std::string& out_sender_ip,
                     uint16_t&    out_sender_port) {
    if (m_fd == INVALID_SOCKET_VAL) return false;

    char buf[RECV_BUF_SIZE];
    sockaddr_in sender{};
    socklen_t   sender_len = sizeof(sender);

    int n = (int)::recvfrom(m_fd, buf, sizeof(buf)-1, 0,
                            (sockaddr*)&sender, &sender_len);
    if (n <= 0) return false;

    buf[n]           = '\0';
    out_msg          = std::string(buf, n);
    out_sender_port  = ntohs(sender.sin_port);

    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender.sin_addr, ipbuf, sizeof(ipbuf));
    out_sender_ip = ipbuf;
    return true;
}

void UDPSocket::close() {
    if (m_fd != INVALID_SOCKET_VAL) {
        CLOSE_SOCKET(m_fd);
        m_fd = INVALID_SOCKET_VAL;
    }
}
