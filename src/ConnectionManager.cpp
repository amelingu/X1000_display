// ConnectionManager.cpp — UDP lifecycle, handshake, UKP dispatch, backlight ticks

#include "ConnectionManager.h"
#include <XPLMUtilities.h>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------

static std::string fieldValue(const std::string& msg, const char* key) {
    std::string needle = std::string(key) + "=";
    size_t pos = msg.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = msg.find('|', pos);
    return msg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// ---------------------------------------------------------------------------

ConnectionManager::ConnectionManager() {
    m_pfd_ep = { "192.168.1.15", BezelSide::PFD };
    m_mfd_ep = { "192.168.1.21", BezelSide::MFD };
}

bool ConnectionManager::init() {
    if (!m_recv_sock.bind(RECV_PORT)) {
        XPLMDebugString("[X1000] ConnectionManager: failed to bind UDP :15683\n");
        return false;
    }
    // Send socket: bind to any port (OS picks one)
    if (!m_send_sock.bind(0)) {
        // Not fatal — fall back to using recv socket for sending
        XPLMDebugString("[X1000] ConnectionManager: send socket bind failed, "
                        "will use recv socket for outbound\n");
    }

    m_ukp.init();
    m_backlight.init();

    XPLMDebugString("[X1000] ConnectionManager: ready on UDP :15683\n");

    // Announce to both iPads
    sendHandshake("ClientComingX", m_pfd_ep);
    sendHandshake("ClientComingX", m_mfd_ep);

    return true;
}

void ConnectionManager::poll() {
    std::string msg, sender_ip;
    uint16_t    sender_port;
    while (m_recv_sock.recv(msg, sender_ip, sender_port))
        handlePacket(msg, sender_ip, sender_port);
}

void ConnectionManager::tickBacklights() {
    m_backlight.tick(m_send_sock,
                     m_pfd_ep.ip, m_mfd_ep.ip,
                     SEND_PORT);
}

void ConnectionManager::shutdown() {
    sendHandshake("ClientOut", m_pfd_ep);
    sendHandshake("ClientOut", m_mfd_ep);
    m_recv_sock.close();
    m_send_sock.close();
    XPLMDebugString("[X1000] ConnectionManager: shutdown.\n");
}

// ---------------------------------------------------------------------------

void ConnectionManager::handlePacket(const std::string& msg,
                                     const std::string& sender_ip,
                                     uint16_t           /*sender_port*/) {
    if (msg.find("ServerStart") == 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "[X1000] ServerStart from %s\n", sender_ip.c_str());
        XPLMDebugString(buf);
        const iPadEndpoint& ep =
            (sender_ip == m_pfd_ep.ip) ? m_pfd_ep : m_mfd_ep;
        sendHandshake("ClientComingX", ep);
        sendHandshake("ClientComingX", ep);
        return;
    }

    if (msg.find("ServerReady") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "[X1000] ServerReady from %s\n", sender_ip.c_str());
        XPLMDebugString(buf);
        return;
    }

    if (msg.find("Answer|A=0") == 0)   return;  // keepalive
    if (msg.find("ServerData|")  == 0) return;  // AP joystick 20 Hz — ignore
    if (msg.find("ServerAv|") == 0 && msg.find("UKP=") == std::string::npos)
        return;  // 1 Hz full G1000 state — ignore

    if (msg.find("ServerAv|UKP=") != std::string::npos) {
        int ukp = parseUKP(msg);
        if (ukp < 0) {
            XPLMDebugString("[X1000] malformed UKP packet\n");
            return;
        }
        m_ukp.handle((uint32_t)ukp, sideForIP(sender_ip));
        return;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "[X1000] unknown packet from %s: %.80s\n",
             sender_ip.c_str(), msg.c_str());
    XPLMDebugString(buf);
}

int ConnectionManager::parseUKP(const std::string& msg) {
    std::string val = fieldValue(msg, "UKP");
    if (val.empty()) return -1;
    try { return std::stoi(val); } catch (...) { return -1; }
}

BezelSide ConnectionManager::sideForIP(const std::string& ip) const {
    return (ip == m_mfd_ep.ip) ? BezelSide::MFD : BezelSide::PFD;
}

void ConnectionManager::sendHandshake(const std::string& msg,
                                      const iPadEndpoint& ep) {
    m_send_sock.send(msg, ep.ip, SEND_PORT);
}
