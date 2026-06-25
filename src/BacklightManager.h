#pragma once
// BacklightManager.h — Reads X-Plane mode datarefs and sends backlight
// state updates to both bezels + audio panel via the Simionic protocol.
//
// OUTBOUND message format (PC → iPad :15684):
//   ClientAv|BL_AP=1|BL_FD=0|BL_HDG=1|BL_NAV=0|...|BL_AUDIO_MIC1=1|...
//
// Each BL_* field is 0 (off) or 1 (lit/active).
// The iPad app forwards the values to the bezel via Bluetooth.
//
// AUDIO PANEL brightness:
//   sim/cockpit2/switches/instrument_brightness_ratio[0..n]
//   We write the index that corresponds to the audio panel backlight.
//   The bezel general brightness is a separate field BL_BRIGHT=0..100.
//
// Individual backlight LEDs (active-mode indicators):
//   AP, FD, HDG, NAV, ALT, VS, FLC, APR, BC, VNV — autopilot annunciators
//   NAV1/NAV2/COM1/COM2/MIC1/MIC2/BOTH — audio panel selectors
//   Each independent, driven by the corresponding XP dataref.

#include "UDPSocket.h"
#include <string>
#include <cstdint>

// One set of backlight states for a single bezel + audio panel
struct BezelLights {
    // Autopilot annunciators
    bool ap    = false;
    bool fd    = false;
    bool hdg   = false;
    bool nav   = false;
    bool alt   = false;
    bool vs    = false;
    bool flc   = false;
    bool apr   = false;
    bool bc    = false;
    bool vnv   = false;

    // Audio panel (PFD bezel only — audio panel is attached there)
    // Active transmit/monitor selectors
    bool audio_com1    = false;
    bool audio_com2    = false;
    bool audio_nav1    = false;
    bool audio_nav2    = false;
    bool audio_mic1    = false;  // pilot mic on COM1
    bool audio_mic2    = false;  // pilot mic on COM2
    bool audio_spkr    = false;  // speaker active

    // General brightness 0–100 (applies to all non-active-mode LEDs)
    uint8_t brightness = 80;

    bool operator==(const BezelLights& o) const;
    bool operator!=(const BezelLights& o) const { return !(*this == o); }
};

class BacklightManager {
public:
    BacklightManager();

    // Call once after datarefs are available.
    void init();

    // Call ~5 Hz from flight loop — reads datarefs, sends updates if changed.
    void tick(UDPSocket& sock,
              const std::string& pfd_ip,
              const std::string& mfd_ip,
              uint16_t send_port);

private:
    // Read current X-Plane state into a BezelLights snapshot
    BezelLights readPFDLights();
    BezelLights readMFDLights();  // MFD has no audio panel LEDs

    // Serialise to ClientAv wire format
    static std::string serialise(const BezelLights& l, bool include_audio);

    // Cached dataref handles (looked up once in init)
    struct Refs {
        // Autopilot engagement
        int* ap_state      = nullptr;  // sim/cockpit2/autopilot/flight_director_mode
        int* fd_state      = nullptr;  // sim/cockpit2/autopilot/flight_director_mode
        int* hdg_mode      = nullptr;  // sim/cockpit2/autopilot/heading_mode
        int* nav_mode      = nullptr;  // sim/cockpit2/autopilot/nav_mode
        int* alt_hold      = nullptr;  // sim/cockpit2/autopilot/altitude_hold_mode
        int* vs_mode       = nullptr;  // sim/cockpit2/autopilot/vvi_status
        int* flc_mode      = nullptr;  // sim/cockpit2/autopilot/airspeed_status
        int* apr_mode      = nullptr;  // sim/cockpit2/autopilot/approach_status
        int* bc_mode       = nullptr;  // sim/cockpit2/autopilot/backcourse_status
        int* vnv_mode      = nullptr;  // sim/cockpit2/autopilot/vnav_status
        // Audio panel (pilot)
        int* audio_com_sel = nullptr;  // sim/cockpit2/radios/actuators/audio_com_selection_pilot
        int* audio_nav_sel = nullptr;  // sim/cockpit2/radios/actuators/audio_nav_selection_pilot
        int* audio_mic_sel = nullptr;  // sim/cockpit2/radios/actuators/audio_selection_pilot
        int* audio_spkr    = nullptr;  // sim/cockpit2/radios/actuators/audio_speaker_enable
        // Brightness
        float* panel_bright = nullptr; // sim/cockpit2/switches/instrument_brightness_ratio[0]
    } m_refs;

    BezelLights m_last_pfd;
    BezelLights m_last_mfd;

    bool m_initialized;

    // Rate limiter
    double m_last_tick_time;
    static constexpr double TICK_INTERVAL = 0.2; // 5 Hz
};
