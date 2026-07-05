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

private:
    void fireCommand(const char* cmd_name);

    bool m_initialized;
    AudioPanelManager m_audio;
};
