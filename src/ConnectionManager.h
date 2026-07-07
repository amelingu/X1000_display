#pragma once
// ConnectionManager.h — UDP connection lifecycle with both iPads.
//
// Owns:
//   - Receive socket (:15683) — UKP events + handshake from both iPads
//   - Send socket           — ClientAv messages to both iPads (:15684)
//     (shared with BacklightManager so it can push BL updates)
//
// Protocol (Simionic, receive side):
//   ServerStart  → send ClientComingX twice
//   ServerReady  → logged
//   ServerAv|UKP=N → dispatch to UKPHandler
//   Answer|A=0   → keepalive, ignore
//   ServerAv|... → full G1000 state at 1 Hz, ignore
//   ServerData|. → AP joystick 20 Hz, ignore
//
// Outbound (BacklightManager uses sendSocket()):
//   ClientAv|BL_AP=1|... → backlight states

#include "UDPSocket.h"
#include "UKPHandler.h"
#include "BacklightManager.h"
#include <string>
#include <cstdint>

struct iPadEndpoint {
    std::string ip;
    BezelSide   side;
};

class ConnectionManager {
public:
    ConnectionManager();

    bool init();
    void poll();           // drain receive buffer — call every frame
    void tickUKP();         // fires held-button repeats
    BezelSide lastUKPSide() const { return m_last_ukp_side; }
    void tickBacklights(); // send backlight updates — call every frame (rate-limited internally)
    void shutdown();

    // Exposed so Plugin.cpp can pass it to BacklightManager
    UDPSocket& sendSocket() { return m_send_sock; }

    const std::string& pfdIP() const { return m_pfd_ep.ip; }
    const std::string& mfdIP() const { return m_mfd_ep.ip; }

private:
    void handlePacket(const std::string& msg,
                      const std::string& sender_ip,
                      uint16_t           sender_port,
                      BezelSide          side);
    static int parseUKP(const std::string& msg);
    void       sendHandshake(const std::string& msg, const iPadEndpoint& ep);

    UDPSocket      m_pfd_recv_sock;  // bound to :15683 — PFD bezel UKP
    UDPSocket      m_mfd_recv_sock;  // bound to :15685 — MFD bezel UKP
    std::string    m_last_ukp_ip;    // IP of last UKP sender (for scan activity)
    UDPSocket      m_send_sock;      // unbound, used for all outbound messages
    UKPHandler     m_ukp;
    BezelSide      m_last_ukp_side = BezelSide::PFD;
    BacklightManager m_backlight;

    iPadEndpoint m_pfd_ep;
    iPadEndpoint m_mfd_ep;

    static constexpr uint16_t PFD_RECV_PORT = 15683;
    static constexpr uint16_t MFD_RECV_PORT = 15685;
    static constexpr uint16_t SEND_PORT      = 15684;
};
