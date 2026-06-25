#pragma once
// UDPSocket.h — Non-blocking UDP socket wrapper (cross-platform)

#include <string>
#include <cstdint>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using socket_t = SOCKET;
   static constexpr socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
   using socket_t = int;
   static constexpr socket_t INVALID_SOCKET_VAL = -1;
#endif

class UDPSocket {
public:
    UDPSocket();
    ~UDPSocket();

    // Bind to a local port for receiving (non-blocking)
    bool bind(uint16_t port);

    // Send a message to a remote host:port
    bool send(const std::string& msg, const std::string& ip, uint16_t port);

    // Receive a message (non-blocking; returns false if nothing available)
    // Fills out_msg, out_sender_ip, out_sender_port on success.
    bool recv(std::string& out_msg,
              std::string& out_sender_ip,
              uint16_t&    out_sender_port);

    void close();
    bool isOpen() const { return m_fd >= 0; }

private:
    socket_t m_fd;
};
