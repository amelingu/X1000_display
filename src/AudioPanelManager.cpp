// AudioPanelManager.cpp — G1000 audio panel command dispatch

#include "AudioPanelManager.h"
#include <XPLMUtilities.h>
#include <XPLMDataAccess.h>
#include <XPLMPlugin.h>
#include <cstdio>
#include <cstring>

AudioPanelManager::AudioPanelManager() {}

static void adjustVolume(float delta) {
    XPLMDataRef dr = XPLMFindDataRef("sim/operation/sound/radio_volume_ratio");
    if (!dr) return;
    float v = XPLMGetDataf(dr) + delta;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    XPLMSetDataf(dr, v);
}

static void fire(const char* cmd_name) {
    XPLMCommandRef cmd = XPLMFindCommand(cmd_name);
    if (cmd) {
        XPLMCommandOnce(cmd);
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[X1000] AudioPanel: command not found: %s\n", cmd_name);
        XPLMDebugString(buf);
    }
}

static void setDisplayBackup(bool on) {
    // On first call, state is unknown — force dataref directly without toggle.
    // On subsequent calls, only toggle if needed.
    static bool s_initialized = false;

    XPLMDataRef dr = XPLMFindDataRef("sim/cockpit2/EFIS/G1000_reversionary_mode");
    if (dr) {
        if (s_initialized) {
            // Check current state and only fire toggle if needed
            int vals[2] = {0, 0};
            XPLMGetDatavi(dr, vals, 0, 2);
            bool currently_on = (vals[0] != 0 || vals[1] != 0);
            if (currently_on != on)
                fire("sim/GPS/G1000_display_reversion");
        } else {
            // First call — we don't know if sim state matches switch position.
            // Read current sim state: if it doesn't match desired, toggle.
            int vals[2] = {0, 0};
            XPLMGetDatavi(dr, vals, 0, 2);
            bool currently_on = (vals[0] != 0 || vals[1] != 0);
            if (currently_on != on)
                fire("sim/GPS/G1000_display_reversion");
            s_initialized = true;
            XPLMDebugString(on ? "[X1000] AudioPanel: Display backup ON (init)\n"
                               : "[X1000] AudioPanel: Display backup OFF (init)\n");
        }
        // Always force the dataref to desired state
        int nv[2] = { on ? 1 : 0, on ? 1 : 0 };
        XPLMSetDatavi(dr, nv, 0, 2);
    } else {
        fire("sim/GPS/G1000_display_reversion");
    }
}

    // Display backup released — MFD NAV page is handled by CLR long press on the bezel

void AudioPanelManager::init() {
    XPLMDebugString("[X1000] AudioPanelManager: initialised\n");
    m_ready = true;
}

void AudioPanelManager::handleUKP(unsigned int ukp) {
    if (!m_ready) return;

    switch (ukp) {
    // MIC transmit selection
    case 43: fire("sim/audio_panel/transmit_audio_com1"); break;  // COM1/MIC
    case 45: fire("sim/audio_panel/transmit_audio_com2"); break;  // COM2/MIC
    case 47: fire("sim/audio_panel/transmit_audio_com3"); break;  // COM3/MIC

    // COM monitor
    case 51: fire("sim/audio_panel/monitor_audio_com1"); break;   // COM1
    case 53: fire("sim/audio_panel/monitor_audio_com2"); break;   // COM2
    case 55: fire("sim/audio_panel/monitor_audio_com3"); break;   // COM3
    case 49: fire("sim/GPS/g1000n1_com12");              break;   // COM1/2

    // NAV monitor
    case 131: fire("sim/audio_panel/monitor_audio_nav1"); break;
    case 133: fire("sim/audio_panel/monitor_audio_nav2"); break;

    // ADF/DME monitor
    case 127: fire("sim/audio_panel/monitor_audio_adf1"); break;
    case 125: fire("sim/audio_panel/monitor_audio_dme");  break;
    case 129: fire("sim/audio_panel/monitor_audio_aux");  break;

    // Speaker
    case 119: fire("sim/audio_panel/toggle_speaker");    break;

    // Marker beacon
    case 121: fire("sim/audio_panel/monitor_audio_mkr");  break;  // MKR/MUTE
    case 123: fire("sim/audio_panel/toggle_hi_sens");     break;

    // Special
    case 115: fire("sim/audio_panel/select_tel");         break;  // TEL
    case 117: fire("sim/audio_panel/select_pa");          break;  // PA
    case 135: fire("sim/audio_panel/toggle_man_squelch"); break;  // MAN SQ
    case 137: fire("sim/audio_panel/play");               break;  // PLAY

    // Pilot/Copilot
    case 139: fire("sim/audio_panel/select_pilot");       break;
    case 141: fire("sim/audio_panel/select_copilot");     break;

    // Display backup switch — stable on/off
    // Even (142) = switch pressed/ON  → set reversionary mode [1,1]
    // Odd  (143) = switch released/OFF → set reversionary mode [0,0]
    case 142: setDisplayBackup(true);  break;  // DISPLAY BACKUP ON
    case 143: setDisplayBackup(false); break;  // DISPLAY BACKUP OFF

    // Volume knob press
    case 145: fire("sim/audio_panel/pilot_volume_push");  break;

    // Volume knobs — adjust sim/operation/sound/radio_volume_ratio directly
    case 146: adjustVolume(+0.05f); break;  // PILOT INNER CW
    case 147: adjustVolume(-0.05f); break;  // PILOT INNER CCW
    case 148: adjustVolume(+0.05f); break;  // PASS OUTER CW
    case 149: adjustVolume(-0.05f); break;  // PASS OUTER CCW

    default: break;
    }
}
