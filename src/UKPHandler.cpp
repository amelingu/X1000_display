// UKPHandler.cpp — Full UKP → X-Plane command dispatch
//
// PFD side  → g1000n1_* commands  (autopilot, NAV, COM, FMS, audio panel)
// MFD side  → g1000n3_* commands  (MFD softkeys, FMS, range)
//
// ==========================================================================
//
// --- Debug logging ---
//
// KNOB_DEBUG_LOG
//   Set to 1 to log every heading/course knob event with elapsed time.
//   Each line shows: UKP value, direction, elapsed ms since last click.
//   Lines ending with * indicate fast-spin mode was triggered.
//   Set to 0 to disable (no performance impact when disabled).
#define KNOB_DEBUG_LOG      0
//
// ==========================================================================
// TUNING PARAMETERS — adjust these to customise bezel feel
// ==========================================================================
//
// --- Heading / Course knob ---
//
// KNOB_NOISE_MS (seconds)
//   Direction noise filter. Opposite-direction clicks arriving within this
//   window are ignored as mechanical noise from fast spinning.
//   Too high: intentional slow direction changes feel sluggish.
//   Too low:  spurious reversals get through during fast spin.
//   Recommended range: 0.06 – 0.15
#define KNOB_NOISE_MS       0.01
//
// KNOB_FAST_THRESHOLD (seconds)
//   Two consecutive same-direction clicks faster than this trigger fast-spin
//   mode (KNOB_FAST_REPEAT commands per click instead of 1).
//   Too high: fast spin triggers at slow speed (may feel jumpy).
//   Too low:  requires very fast spin to trigger (bug moves slowly).
//   Must be <= KNOB_NOISE_MS to avoid filtering valid fast clicks.
//   Recommended range: 0.06 – 0.15
#define KNOB_FAST_THRESHOLD 0.15
//
// KNOB_FAST_REPEAT (integer)
//   Number of X-Plane commands fired per click in fast-spin mode.
//   Higher = bug moves faster when spinning quickly.
//   Recommended range: 5 – 15
#define KNOB_FAST_REPEAT    10
//
// --- Hold-repeat keys (NOSE UP/DOWN, FMS cursor) ---
//
// HOLD_DELAY_S (seconds)
//   How long a key must be held before auto-repeat starts.
//   Recommended range: 0.3 – 0.8
#define HOLD_DELAY_S        0.5
//
// HOLD_RATE_S (seconds between repeats)
//   Initial repeat rate (1 / HOLD_RATE_S = commands per second).
//   0.2 = 5/sec. Only applies to NOSE UP/DOWN (cursor keys accelerate).
//   Recommended range: 0.1 – 0.3
#define HOLD_RATE_S         0.2
//
// --- Cursor key acceleration ---
//   Cursor keys start at HOLD_RATE_S and accelerate over time.
//   Tiers: 0-1s held → 5/sec, 1-2s → 10/sec, 2-3s → 15/sec, 3s+ → 20/sec
//   These are currently hardcoded in UKPHandler::tick() rates[] array.
//
// ==========================================================================

#include "UKPHandler.h"
#include "Platform.h"
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
    m[92]  = { "sim/GPS/g1000n1_ap",        "sim/GPS/g1000n3_ap"        };
    m[64]  = { "sim/GPS/g1000n1_fd",        "sim/GPS/g1000n3_fd"        };
    m[100] = { "sim/GPS/g1000n1_hdg",       "sim/GPS/g1000n3_hdg"       };
    m[66]  = { "sim/GPS/g1000n1_alt",       "sim/GPS/g1000n3_alt"       };
    m[98]  = { "sim/GPS/g1000n1_nav",       "sim/GPS/g1000n3_nav"       };
    m[60]  = { "sim/GPS/g1000n1_vnv",       "sim/GPS/g1000n3_vnv"       };
    m[96]  = { "sim/GPS/g1000n1_apr",       "sim/GPS/g1000n3_apr"       };
    m[58]  = { "sim/GPS/g1000n1_bc",        "sim/GPS/g1000n3_bc"        };
    m[94]  = { "sim/GPS/g1000n1_vs",        "sim/GPS/g1000n3_vs"        };
    m[56]  = { "sim/GPS/g1000n1_nose_up",   "sim/GPS/g1000n3_nose_up"   };
    m[90]  = { "sim/GPS/g1000n1_flc",       "sim/GPS/g1000n3_flc"       };
    m[62]  = { "sim/GPS/g1000n1_nose_down", "sim/GPS/g1000n3_nose_down" };

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
    m[32]  = { "sim/GPS/g1000n1_pan_push",  "sim/GPS/g1000n3_pan_push"  };
    m[198] = { "sim/GPS/g1000n1_pan_up",    "sim/GPS/g1000n3_pan_up"    };
    m[200] = { "sim/GPS/g1000n1_pan_down",  "sim/GPS/g1000n3_pan_down"  };
    m[194] = { "sim/GPS/g1000n1_pan_left",  "sim/GPS/g1000n3_pan_left"  };
    m[196] = { "sim/GPS/g1000n1_pan_right", "sim/GPS/g1000n3_pan_right" };

    // -----------------------------------------------------------------------
    // FMS / NAVIGATION — both sides have FMS knob and nav buttons
    // -----------------------------------------------------------------------
    m[210] = { "sim/GPS/g1000n1_direct",    "sim/GPS/g1000n3_direct"    };
    m[184] = { "sim/GPS/g1000n1_menu",      "sim/GPS/g1000n3_menu"      };
    m[212] = { "sim/GPS/g1000n1_fpl",       "sim/GPS/g1000n3_fpl"       };
    m[182] = { "sim/GPS/g1000n1_proc",      "sim/GPS/g1000n3_proc"      };
    // CLR fires on release (odd), not press (even)
    m[215] = { "sim/GPS/g1000n1_clr",       "sim/GPS/g1000n3_clr"       };  // CLR release
    m[180] = { "sim/GPS/g1000n1_ent",       "sim/GPS/g1000n3_ent"       };
    m[190] = { "sim/GPS/g1000n1_cursor",    "sim/GPS/g1000n3_cursor"    };

    // -----------------------------------------------------------------------
    // SOFTKEYS SK01–SK12
    // PFD softkeys: UKP 156,158,...,178  → g1000n1_softkey1..12
    // MFD softkeys: same UKP range but from MFD iPad → g1000n3_softkey1..12
    // -----------------------------------------------------------------------
    for (int i = 0; i < 12; ++i) {
        uint32_t ukp = 156 + (uint32_t)(i * 2);
        char pfd[64], mfd_cmd[64];
        snprintf(pfd,     sizeof(pfd),     "sim/GPS/g1000n1_softkey%d", i + 1);
        snprintf(mfd_cmd, sizeof(mfd_cmd), "sim/GPS/g1000n3_softkey%d", i + 1);
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
    m[40]  = { "sim/GPS/g1000n1_fms_outer_up",   "sim/GPS/g1000n3_fms_outer_up"   };
    m[41]  = { "sim/GPS/g1000n1_fms_outer_down",  "sim/GPS/g1000n3_fms_outer_down" };
    m[38]  = { "sim/GPS/g1000n1_fms_inner_up",   "sim/GPS/g1000n3_fms_inner_up"   };
    m[39]  = { "sim/GPS/g1000n1_fms_inner_down",  "sim/GPS/g1000n3_fms_inner_down" };

    // RANGE / cursor — both sides
    m[192] = { "sim/GPS/g1000n1_range_up",        "sim/GPS/g1000n3_range_up"       };
    m[193] = { "sim/GPS/g1000n1_range_down",       "sim/GPS/g1000n3_range_down"     };

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
    // AUDIO PANEL (physically attached to PFD bezel)
    // UKP values confirmed by BLE capture session 2026-07-04.
    //
    // Note: X-Plane 12 G1000 audio panel buttons are exposed as commands
    // under sim/GPS/g1000n1_*. Volume knobs are dataref-only in default C172.
    //
    // Audio panel buttons send only the release (odd) value — map those
    // MIC select buttons
    m[43]  = { "sim/GPS/g1000n1_com1_mic",        "" };  // COM1/MIC release
    m[45]  = { "sim/GPS/g1000n1_com2_mic",        "" };  // COM2/MIC release
    m[47]  = { "sim/GPS/g1000n1_com3_mic",        "" };  // COM3/MIC release
    // COM monitor buttons
    m[51]  = { "sim/GPS/g1000n1_com1",            "" };  // COM1 monitor release
    m[53]  = { "sim/GPS/g1000n1_com2",            "" };  // COM2 monitor release
    m[55]  = { "sim/GPS/g1000n1_com3",            "" };  // COM3 monitor release
    // COM1/2 toggle
    m[49]  = { "sim/GPS/g1000n1_com12",           "" };  // COM1/2 release
    // NAV monitor buttons
    m[131] = { "sim/GPS/g1000n1_nav1",            "" };  // NAV1 release
    m[133] = { "sim/GPS/g1000n1_nav2",            "" };  // NAV2 release
    m[127] = { "sim/GPS/g1000n1_adf",             "" };  // ADF release
    m[125] = { "sim/GPS/g1000n1_dme",             "" };  // DME release
    m[129] = { "sim/GPS/g1000n1_aux",             "" };  // AUX release
    // Speaker / headphone
    m[119] = { "sim/GPS/g1000n1_spkr",            "" };  // SPKR release
    // Marker beacon
    m[121] = { "sim/GPS/g1000n1_mkr_mute",        "" };  // MKR/MUTE release
    m[123] = { "sim/GPS/g1000n1_hi_sens",         "" };  // HI SENS release
    // Special buttons
    m[115] = { "sim/GPS/g1000n1_tel",             "" };  // TEL release
    m[117] = { "sim/GPS/g1000n1_pa",              "" };  // PA release
    m[135] = { "sim/GPS/g1000n1_man_sq",          "" };  // MAN SQ release
    m[137] = { "sim/GPS/g1000n1_play",            "" };  // PLAY release
    // Pilot/Copilot select
    m[139] = { "sim/GPS/g1000n1_pilot",           "" };  // PILOT release
    m[141] = { "sim/GPS/g1000n1_coplt",           "" };  // COPILOT release
    // Display backup
    m[143] = { "sim/GPS/g1000n1_display_backup",  "" };  // DISPLAY BACKUP release
    // Pilot volume knob press
    m[145] = { "sim/GPS/g1000n1_pilot_knob",      "" };  // PILOT KNOB PRESS release
    // Pilot inner volume knob CW/CCW (knobs send odd values too)
    m[146] = { "sim/GPS/g1000n1_pilot_vol_up",    "" };  // PILOT INNER CW
    m[147] = { "sim/GPS/g1000n1_pilot_vol_dn",    "" };  // PILOT INNER CCW
    // Passenger outer volume knob CW/CCW
    m[148] = { "sim/GPS/g1000n1_pass_vol_up",     "" };  // PASS OUTER CW
    m[149] = { "sim/GPS/g1000n1_pass_vol_dn",     "" };  // PASS OUTER CCW
    // -----------------------------------------------------------------------

    return m;
}

// ---------------------------------------------------------------------------

static std::unordered_map<uint32_t, CmdPair> s_map;

UKPHandler::UKPHandler() : m_initialized(false) {}

void UKPHandler::init() {
    s_map = buildMap();
    m_initialized = true;
    m_audio.init();
    XPLMDebugString("[X1000] UKPHandler: initialised (PFD+MFD+audio panel map)\n");
}

// ---------------------------------------------------------------------------
// Knob direction filter — prevents spurious reversals on fast spins
// Fires each click immediately but ignores opposite-direction clicks
// that arrive within a short noise window after the last click.
// ---------------------------------------------------------------------------

struct KnobFilter {
    double last_time  = 0.0;
    bool   last_cw    = true;
    double noise_ms   = KNOB_NOISE_MS;
};

static void tickKnob(KnobFilter& k, bool cw,
                     const char* cmd_up, const char* cmd_dn) {
    double t = Platform::now_seconds();
    double dt = t - k.last_time;

    // Ignore direction reversal if it arrives too quickly after last click
    if (dt < k.noise_ms && cw != k.last_cw) {
#if KNOB_DEBUG_LOG
        char dbuf[80];
        snprintf(dbuf, sizeof(dbuf),
                 "[X1000] KNOB: UKP=%s dt=%.1fms filtered\n",
                 cw ? "CW " : "CCW", dt * 1000.0);
        XPLMDebugString(dbuf);
#endif
        return;  // noise — skip
    }

    // Detect fast spin: clicks arriving faster than 80ms = fast spin
    // Fire 5 commands per click when spinning fast, 1 when slow
    bool fast = (dt < KNOB_FAST_THRESHOLD && cw == k.last_cw);
    int repeat = fast ? KNOB_FAST_REPEAT : 1;

    k.last_time = t;
    k.last_cw   = cw;

#if KNOB_DEBUG_LOG
    {
        char dbuf[80];
        snprintf(dbuf, sizeof(dbuf),
                 "[X1000] KNOB: UKP=%s dt=%.1fms%s\n",
                 cw ? "CW " : "CCW",
                 dt * 1000.0,
                 fast ? " *" : "");
        XPLMDebugString(dbuf);
    }
#endif

    const char* cmd = cw ? cmd_up : cmd_dn;
    XPLMCommandRef ref = XPLMFindCommand(cmd);
    if (ref) {
        for (int i = 0; i < repeat; ++i)
            XPLMCommandOnce(ref);
    }
}

// ---------------------------------------------------------------------------
// Hold-repeat keys — press fires once immediately, held fires at 5Hz after 0.5s
// ---------------------------------------------------------------------------

static UKPHandler::HoldKey s_hold_keys[] = {
    // NOSE UP/DOWN — no acceleration needed, fixed 5/sec
    { 56, 57, "sim/GPS/g1000n1_nose_up",   "sim/GPS/g1000n3_nose_up",   BezelSide::PFD, false, 0, 0, HOLD_DELAY_S, HOLD_RATE_S },
    { 62, 63, "sim/GPS/g1000n1_nose_down", "sim/GPS/g1000n3_nose_down", BezelSide::PFD, false, 0, 0, HOLD_DELAY_S, HOLD_RATE_S },
    // CURSOR keys — with acceleration (starts 5/sec, +5/sec every second, cap 20/sec)
    { 198, 199, "sim/GPS/g1000n1_pan_up",    "sim/GPS/g1000n3_pan_up",    BezelSide::PFD, false, 0, 0, HOLD_DELAY_S, HOLD_RATE_S },
    { 200, 201, "sim/GPS/g1000n1_pan_down",  "sim/GPS/g1000n3_pan_down",  BezelSide::PFD, false, 0, 0, HOLD_DELAY_S, HOLD_RATE_S },
    { 194, 195, "sim/GPS/g1000n1_pan_left",  "sim/GPS/g1000n3_pan_left",  BezelSide::PFD, false, 0, 0, HOLD_DELAY_S, HOLD_RATE_S },
    { 196, 197, "sim/GPS/g1000n1_pan_right", "sim/GPS/g1000n3_pan_right", BezelSide::PFD, false, 0, 0, HOLD_DELAY_S, HOLD_RATE_S },
};
static constexpr int N_HOLD_KEYS = 6;
// Cursor keys start at index 2 — these get acceleration
static constexpr int CURSOR_KEY_START = 2;

static bool handleHoldKey(uint32_t ukp, BezelSide side) {
    for (int i = 0; i < N_HOLD_KEYS; ++i) {
        auto& k = s_hold_keys[i];
        if (ukp == k.press_ukp) {
            // Press — fire immediately and start hold timer
            const char* cmd = (side == BezelSide::MFD) ? k.mfd_cmd : k.pfd_cmd;
            XPLMCommandRef ref = XPLMFindCommand(cmd);
            if (ref) XPLMCommandOnce(ref);
            k.held       = true;
            k.side       = side;
            k.press_time = Platform::now_seconds();
            k.last_fire  = k.press_time;
            return true;
        }
        if (ukp == k.release_ukp && k.held) {
            k.held = false;
            return true;
        }
    }
    return false;
}

void UKPHandler::handle(uint32_t ukp, BezelSide side) {
    // Check hold-repeat keys first
    if (handleHoldKey(ukp, side)) return;

    // Route audio panel UKPs to AudioPanelManager (PFD side only)
    if (side == BezelSide::PFD &&
        ((ukp >= 43 && ukp <= 55) ||
         (ukp >= 115 && ukp <= 135) ||
         (ukp >= 139 && ukp <= 149))) {
        m_audio.handleUKP(ukp);
        return;
    }

    // MFD CLR press (214) — force MFD to NAV full screen, but only when
    // DISPLAY BACKUP is OFF (MFD is in normal mode, not showing backup PFD)
    if (side == BezelSide::MFD && ukp == 214) {
        XPLMDataRef rev_dr = XPLMFindDataRef("sim/cockpit2/EFIS/G1000_reversionary_mode");
        bool backup_active = false;
        if (rev_dr) {
            int vals[2] = {0, 0};
            XPLMGetDatavi(rev_dr, vals, 0, 2);
            backup_active = (vals[0] || vals[1]);
        }
        if (!backup_active) {
            XPLMDataRef page_dr = XPLMFindDataRef("sim/cockpit/g1000/g1000_n2_page");
            if (page_dr) {
                XPLMSetDatai(page_dr, 0);
                XPLMDebugString("[X1000] MFD CLR held — forced to NAV full screen\n");
            }
        }
        return;
    }
    if (!m_initialized) return;

    // Fast-spin knob accumulators (static, persist between calls)
    static KnobFilter hdg_filter, crs_filter;
    static KnobFilter mfd_hdg_filter, mfd_crs_filter;
    if (side == BezelSide::PFD) {
        if (ukp == 68 || ukp == 69) {
            tickKnob(hdg_filter, ukp == 68,
                     "sim/GPS/g1000n1_hdg_up", "sim/GPS/g1000n1_hdg_down");
            return;
        }
        if (ukp == 74 || ukp == 75) {
            tickKnob(crs_filter, ukp == 74,
                     "sim/GPS/g1000n1_crs_up", "sim/GPS/g1000n1_crs_down");
            return;
        }
    }
    if (side == BezelSide::MFD) {
        if (ukp == 68 || ukp == 69) {
            tickKnob(mfd_hdg_filter, ukp == 68,
                     "sim/GPS/g1000n3_hdg_up", "sim/GPS/g1000n3_hdg_down");
            return;
        }
        if (ukp == 74 || ukp == 75) {
            tickKnob(mfd_crs_filter, ukp == 74,
                     "sim/GPS/g1000n3_crs_up", "sim/GPS/g1000n3_crs_down");
            return;
        }
    }

    auto it = s_map.find(ukp);
    if (it == s_map.end()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "[X1000] UKP %u — no mapping (side=%s)\n",
                 ukp, side == BezelSide::PFD ? "PFD" : "MFD");
        XPLMDebugString(buf);
        return;
    }

    const std::string& cmd_str = (side == BezelSide::MFD && !it->second.mfd.empty())
                                  ? it->second.mfd : it->second.pfd;
    if (cmd_str.empty()) return;

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

void UKPHandler::tick() {
    double now = Platform::now_seconds();
    for (int i = 0; i < N_HOLD_KEYS; ++i) {
        auto& k = s_hold_keys[i];
        if (!k.held) continue;
        double held_for = now - k.press_time;
        // Wait for initial delay before repeating
        if (held_for < k.delay_s) continue;

        // Compute current rate with acceleration for cursor keys
        double rate = k.rate_s;
        if (i >= CURSOR_KEY_START) {
            // Accelerate: +5/sec every second held (after initial delay)
            // 0-1s: 5/sec (rate=0.20s), 1-2s: 10/sec (rate=0.10s),
            // 2-3s: 15/sec (rate=0.067s), 3s+: 20/sec (rate=0.05s, cap)
            double accel_time = held_for - k.delay_s;
            int tier = (int)(accel_time);  // 0,1,2,3...
            if (tier > 3) tier = 3;        // cap at tier 3 = 20/sec
            double rates[] = { 0.20, 0.10, 0.067, 0.05 };
            rate = rates[tier];
        }

        if (now - k.last_fire >= rate) {
            const char* cmd = (k.side == BezelSide::MFD) ? k.mfd_cmd : k.pfd_cmd;
            XPLMCommandRef ref = XPLMFindCommand(cmd);
            if (ref) XPLMCommandOnce(ref);
            k.last_fire = now;
        }
    }
}
