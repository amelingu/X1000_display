// UKPHandler.cpp — Full UKP → X-Plane command dispatch
//
// PFD side  → g1000n1_* commands  (autopilot, NAV, COM, FMS, audio panel)
// MFD side  → g1000n2_* commands  (MFD softkeys, FMS, range)
//
// Audio panel UKP values (148/149, 146/147 AUDIO VOL and panel buttons)
// are mapped to sim/cockpit2/radios/* datarefs where X-Plane has no
// direct command — those are marked TODO for dataref-write path.

#include "UKPHandler.h"
#include <XPLMUtilities.h>
#include <XPLMDataAccess.h>
#include <cstdio>
#include <unordered_map>
#include <string>

// ---------------------------------------------------------------------------
// Map entry — one UKP can have different commands on PFD vs MFD side.
// An empty string means "no command known for this side".
// ---------------------------------------------------------------------------
struct CmdPair {
    std::string pfd;
    std::string mfd;
};

static std::unordered_map<uint32_t, CmdPair> buildMap() {
    std::unordered_map<uint32_t, CmdPair> m;

    // -----------------------------------------------------------------------
    // AUTOPILOT / FLIGHT DIRECTOR — PFD bezel only; same button exists on MFD
    // bezel in some configurations so we map n2_* defensively.
    // -----------------------------------------------------------------------
    m[92]  = { "sim/GPS/g1000n1_ap",        "sim/GPS/g1000n2_ap"        };
    m[64]  = { "sim/GPS/g1000n1_fd",        "sim/GPS/g1000n2_fd"        };
    m[100] = { "sim/GPS/g1000n1_hdg",       "sim/GPS/g1000n2_hdg"       };
    m[66]  = { "sim/GPS/g1000n1_alt",       "sim/GPS/g1000n2_alt"       };
    m[98]  = { "sim/GPS/g1000n1_nav",       "sim/GPS/g1000n2_nav"       };
    m[60]  = { "sim/GPS/g1000n1_vnv",       "sim/GPS/g1000n2_vnv"       };
    m[96]  = { "sim/GPS/g1000n1_apr",       "sim/GPS/g1000n2_apr"       };
    m[58]  = { "sim/GPS/g1000n1_bc",        "sim/GPS/g1000n2_bc"        };
    m[94]  = { "sim/GPS/g1000n1_vs",        "sim/GPS/g1000n2_vs"        };
    m[56]  = { "sim/GPS/g1000n1_nose_up",   "sim/GPS/g1000n2_nose_up"   };
    m[90]  = { "sim/GPS/g1000n1_flc",       "sim/GPS/g1000n2_flc"       };
    m[62]  = { "sim/GPS/g1000n1_nose_down", "sim/GPS/g1000n2_nose_down" };

    // -----------------------------------------------------------------------
    // NAV / COM — PFD only (radio heads only on PFD bezel)
    // -----------------------------------------------------------------------
    // NAV vol push, standby flip, dual knob push
    m[78]  = { "sim/GPS/g1000n1_nvol",      "" };
    m[74]  = { "sim/GPS/g1000n1_nav_ff",    "" };
    m[84]  = { "sim/GPS/g1000n1_nav12",     "" };

    // COM vol push, standby flip, dual knob push
    m[206] = { "sim/GPS/g1000n1_cvol",      "" };
    m[202] = { "sim/GPS/g1000n1_com_ff",    "" };
    m[110] = { "sim/GPS/g1000n1_com12",     "" };

    // HDG knob push (sync), CRS knob push (sync)
    m[86]  = { "sim/GPS/g1000n1_hdg_sync",  "" };
    m[112] = { "sim/GPS/g1000n1_crs_sync",  "" };
    // UKP 88 = ALT knob push — no confirmed XP command; add when found

    // -----------------------------------------------------------------------
    // CURSOR / PAN — both sides have a joystick/cursor on their bezel
    // -----------------------------------------------------------------------
    m[32]  = { "sim/GPS/g1000n1_pan_push",  "sim/GPS/g1000n2_pan_push"  };
    m[198] = { "sim/GPS/g1000n1_pan_up",    "sim/GPS/g1000n2_pan_up"    };
    m[200] = { "sim/GPS/g1000n1_pan_down",  "sim/GPS/g1000n2_pan_down"  };
    m[194] = { "sim/GPS/g1000n1_pan_left",  "sim/GPS/g1000n2_pan_left"  };
    m[196] = { "sim/GPS/g1000n1_pan_right", "sim/GPS/g1000n2_pan_right" };

    // -----------------------------------------------------------------------
    // FMS / NAVIGATION — both sides have FMS knob and nav buttons
    // -----------------------------------------------------------------------
    m[210] = { "sim/GPS/g1000n1_direct",    "sim/GPS/g1000n2_direct"    };
    m[184] = { "sim/GPS/g1000n1_menu",      "sim/GPS/g1000n2_menu"      };
    m[212] = { "sim/GPS/g1000n1_fpl",       "sim/GPS/g1000n2_fpl"       };
    m[182] = { "sim/GPS/g1000n1_proc",      "sim/GPS/g1000n2_proc"      };
    m[214] = { "sim/GPS/g1000n1_clr",       "sim/GPS/g1000n2_clr"       };
    m[180] = { "sim/GPS/g1000n1_ent",       "sim/GPS/g1000n2_ent"       };
    m[190] = { "sim/GPS/g1000n1_cursor",    "sim/GPS/g1000n2_cursor"    };

    // -----------------------------------------------------------------------
    // SOFTKEYS SK01–SK12
    // PFD softkeys: UKP 156,158,...,178  → g1000n1_softkey1..12
    // MFD softkeys: same UKP range but from MFD iPad → g1000n2_softkey1..12
    // -----------------------------------------------------------------------
    for (int i = 0; i < 12; ++i) {
        uint32_t ukp = 156 + (uint32_t)(i * 2);
        char pfd[64], mfd_cmd[64];
        snprintf(pfd,     sizeof(pfd),     "sim/GPS/g1000n1_softkey%d", i + 1);
        snprintf(mfd_cmd, sizeof(mfd_cmd), "sim/GPS/g1000n2_softkey%d", i + 1);
        m[ukp] = { pfd, mfd_cmd };
    }

    // -----------------------------------------------------------------------
    // KNOB ROTATIONS — CW (even) and CCW (odd) both fire
    // -----------------------------------------------------------------------

    // NAV vol
    m[76]  = { "sim/GPS/g1000n1_nvol_up",        "" };
    m[77]  = { "sim/GPS/g1000n1_nvol_dn",         "" };

    // NAV outer / inner
    m[82]  = { "sim/GPS/g1000n1_nav_outer_up",   "" };
    m[83]  = { "sim/GPS/g1000n1_nav_outer_down",  "" };
    m[80]  = { "sim/GPS/g1000n1_nav_inner_up",   "" };
    m[81]  = { "sim/GPS/g1000n1_nav_inner_down",  "" };

    // HDG
    m[68]  = { "sim/GPS/g1000n1_hdg_up",         "" };
    m[69]  = { "sim/GPS/g1000n1_hdg_down",        "" };

    // ALT outer / inner
    m[72]  = { "sim/GPS/g1000n1_alt_outer_up",   "" };
    m[73]  = { "sim/GPS/g1000n1_alt_outer_down",  "" };
    m[70]  = { "sim/GPS/g1000n1_alt_inner_up",   "" };
    m[71]  = { "sim/GPS/g1000n1_alt_inner_down",  "" };

    // FMS outer / inner — both sides
    m[40]  = { "sim/GPS/g1000n1_fms_outer_up",   "sim/GPS/g1000n2_fms_outer_up"   };
    m[41]  = { "sim/GPS/g1000n1_fms_outer_down",  "sim/GPS/g1000n2_fms_outer_down" };
    m[38]  = { "sim/GPS/g1000n1_fms_inner_up",   "sim/GPS/g1000n2_fms_inner_up"   };
    m[39]  = { "sim/GPS/g1000n1_fms_inner_down",  "sim/GPS/g1000n2_fms_inner_down" };

    // RANGE / cursor — both sides
    m[192] = { "sim/GPS/g1000n1_range_up",        "sim/GPS/g1000n2_range_up"       };
    m[193] = { "sim/GPS/g1000n1_range_down",       "sim/GPS/g1000n2_range_down"     };

    // BARO — PFD only
    m[102] = { "sim/GPS/g1000n1_baro_up",         "" };
    m[103] = { "sim/GPS/g1000n1_baro_down",        "" };

    // CRS — PFD only
    m[104] = { "sim/GPS/g1000n1_crs_up",          "" };
    m[105] = { "sim/GPS/g1000n1_crs_down",         "" };

    // COM outer / inner
    m[108] = { "sim/GPS/g1000n1_com_outer_up",    "" };
    m[109] = { "sim/GPS/g1000n1_com_outer_down",   "" };
    m[106] = { "sim/GPS/g1000n1_com_inner_up",    "" };
    m[107] = { "sim/GPS/g1000n1_com_inner_down",   "" };

    // COM vol
    m[36]  = { "sim/GPS/g1000n1_cvol_up",         "" };
    m[37]  = { "sim/GPS/g1000n1_cvol_dn",          "" };

    // -----------------------------------------------------------------------
    // AUDIO PANEL (physically on PFD bezel, UKPs arrive from PFD iPad)
    //
    // The Simionic handover listed these as "no command found".
    // X-Plane 12 exposes audio panel via sim/cockpit2/radios/actuators/
    // and sim/audio_panel/* — most are DATAREFS not commands, so the
    // BacklightManager / a dedicated AudioPanelHandler should write them.
    //
    // The two volume knobs do have commands in some aircraft:
    //   sim/audio_panel/select_pilot       (pilot audio selector)
    //   sim/audio_panel/monitor_volume_up/dn  (not standard in XP12 default)
    //
    // For now we log these UKPs at INFO level.  The BacklightManager will
    // track COM/NAV active states independently via datarefs.
    //
    // AUDIO VOL outer CW/CCW
    m[148] = { "TODO:audio_vol_outer_up",   "" };
    m[149] = { "TODO:audio_vol_outer_dn",   "" };
    // AUDIO VOL inner CW/CCW
    m[146] = { "TODO:audio_vol_inner_up",   "" };
    m[147] = { "TODO:audio_vol_inner_dn",   "" };
    // -----------------------------------------------------------------------

    return m;
}

// ---------------------------------------------------------------------------

static std::unordered_map<uint32_t, CmdPair> s_map;

UKPHandler::UKPHandler() : m_initialized(false) {}

void UKPHandler::init() {
    s_map = buildMap();
    m_initialized = true;
    XPLMDebugString("[X1000] UKPHandler: initialised (PFD+MFD+audio panel map)\n");
}

void UKPHandler::handle(uint32_t ukp, BezelSide side) {
    if (!m_initialized) return;

    auto it = s_map.find(ukp);
    if (it == s_map.end()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "[X1000] UKP %u — no mapping (side=%s)\n",
                 ukp, side == BezelSide::PFD ? "PFD" : "MFD");
        XPLMDebugString(buf);
        return;
    }

    const CmdPair& pair = it->second;
    const std::string& cmd_str = (side == BezelSide::MFD && !pair.mfd.empty())
                                 ? pair.mfd
                                 : pair.pfd;

    if (cmd_str.empty()) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "[X1000] UKP %u — no command for %s side\n",
                 ukp, side == BezelSide::MFD ? "MFD" : "PFD");
        XPLMDebugString(buf);
        return;
    }

    // Skip TODO placeholders (audio panel, unresolved)
    if (cmd_str.rfind("TODO:", 0) == 0) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "[X1000] UKP %u — audio panel (%s) not yet mapped\n",
                 ukp, cmd_str.c_str());
        XPLMDebugString(buf);
        return;
    }

    fireCommand(cmd_str.c_str());
}

void UKPHandler::fireCommand(const char* cmd_name) {
    XPLMCommandRef cmd = XPLMFindCommand(cmd_name);
    if (!cmd) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[X1000] fireCommand: NOT FOUND — %s\n", cmd_name);
        XPLMDebugString(buf);
        return;
    }
    XPLMCommandOnce(cmd);
}
