#pragma once
#include "AudioPanelManager.h"
// UKPHandler.h — Maps Simionic bezel UKP values → X-Plane commands
//
// Protocol rules:
//   Even UKP  = button PRESSED  → fire command
//   Odd  UKP  = button RELEASED → ignore
//   Knob rotation = SINGLE UKP per click (no release event)
//   Even rotation UKP = CW click, Odd = CCW click
//
// Both PFD and MFD bezels now each connect to their own iPad via Bluetooth,
// so both sides send UKP events.  PFD → g1000n1_*, MFD → g1000n2_*.
// Audio panel is physically attached to the PFD bezel; its UKPs arrive
// from the PFD iPad and map to sim/cockpit2/radios/* and sim/audio_panel/*.

#include <cstdint>
#include <string>

enum class BezelSide { PFD, MFD };

class UKPHandler {
public:
    UKPHandler();

    // Call once after X-Plane has loaded an aircraft (datarefs/commands ready).
    void init();

    // Process one UKP value from a bezel packet.
    void handle(uint32_t ukp, BezelSide side);
    void tick();  // called every flight loop frame — fires held-button repeats

    // Hold-repeat state — public so Plugin.cpp flight loop can call tick()
    struct HoldKey {
        uint32_t    press_ukp  = 0;      // even UKP that started the hold
        uint32_t    release_ukp = 0;     // odd UKP that ends the hold
        const char* pfd_cmd    = nullptr;
        const char* mfd_cmd    = nullptr;
        BezelSide   side       = BezelSide::PFD;
        bool        held       = false;
        double      press_time = 0.0;
        double      last_fire  = 0.0;
        double      delay_s    = 0.5;   // initial delay before repeat
        double      rate_s     = 0.2;   // interval between repeats (5/sec)
    };

private:
    void fireCommand(const char* cmd_name);

    bool m_initialized;
    AudioPanelManager m_audio;
};
