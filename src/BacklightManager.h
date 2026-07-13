#pragma once
// BacklightManager.h — Reads X-Plane datarefs and sends LED state to bezels.
//
// Sends a binary packet to x1000_bezel.py on port 15684:
//   Byte 0:     brightness (0=off, 64=max)
//   Bytes 1..N: UKP release values for active button LEDs

#include "UDPSocket.h"
#include <XPLMDataAccess.h>
#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// BezelLights — current LED state snapshot
// ---------------------------------------------------------------------------

struct BezelLights {
    // Brightness (0=off, 64=full)
    uint8_t brightness = 0;

    // Audio panel button LEDs (PFD bezel only)
    bool audio_com1 = false;   // COM1 monitor
    bool audio_com2 = false;   // COM2 monitor
    bool audio_nav1 = false;   // NAV1 monitor
    bool audio_nav2 = false;   // NAV2 monitor
    bool audio_adf1 = false;   // ADF monitor
    bool audio_dme1 = false;   // DME monitor
    bool audio_mkr  = false;   // Marker beacon monitor
    bool audio_mic1 = false;   // COM1 MIC selected
    bool audio_mic2 = false;   // COM2 MIC selected
    bool audio_spkr = false;   // Speaker active

    bool operator==(const BezelLights& o) const;
    bool operator!=(const BezelLights& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------

class BacklightManager {
public:
    BacklightManager();
    void init();
    void tick(UDPSocket& sock,
              const std::string& pfd_ip,
              const std::string& mfd_ip,
              uint16_t           send_port);

private:
    BezelLights readPFDLights();
    BezelLights readMFDLights();

    struct Refs {
        // Audio panel datarefs
        XPLMDataRef audio_sel_com1 = nullptr;
        XPLMDataRef audio_sel_com2 = nullptr;
        XPLMDataRef audio_sel_nav1 = nullptr;
        XPLMDataRef audio_sel_nav2 = nullptr;
        XPLMDataRef audio_sel_adf1 = nullptr;
        XPLMDataRef audio_sel_dme1 = nullptr;
        XPLMDataRef audio_spkr     = nullptr;
        XPLMDataRef audio_mkr      = nullptr;
        XPLMDataRef audio_com_sel  = nullptr;  // 6=COM1 mic, 7=COM2 mic
        // Brightness
        float*      panel_bright   = nullptr;
    } m_refs;

    BezelLights m_last_pfd;
    BezelLights m_last_mfd;
    bool        m_initialized;
    double      m_last_tick_time;

    static constexpr double TICK_INTERVAL = 0.2;   // 5 Hz
};
