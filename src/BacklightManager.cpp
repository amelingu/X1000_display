// BacklightManager.cpp — Reads X-Plane autopilot / audio datarefs and
// pushes backlight state updates to both bezels at 5 Hz.
//
// Wire format sent to each iPad on port 15684:
//   ClientAv|BL_AP=1|BL_FD=0|BL_HDG=1|BL_NAV=0|BL_ALT=1|BL_VS=0|
//            BL_FLC=0|BL_APR=0|BL_BC=0|BL_VNV=0|BL_BRIGHT=80
//
// PFD message additionally includes audio panel LEDs:
//   ...|BL_AUDIO_COM1=1|BL_AUDIO_COM2=0|BL_AUDIO_NAV1=0|BL_AUDIO_NAV2=0|
//       BL_AUDIO_MIC1=1|BL_AUDIO_MIC2=0|BL_AUDIO_SPKR=0
//
// The iPad app parses these and forwards to the bezel via Bluetooth.
// Individual backlights are independent; BL_BRIGHT is the general
// non-active-mode LED intensity (0–100).

#include "BacklightManager.h"
#include "Platform.h"
#include <XPLMDataAccess.h>
#include <XPLMUtilities.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// now_seconds() from Platform::

static int readInt(XPLMDataRef dr) {
    return dr ? XPLMGetDatai(dr) : 0;
}

static float readFloat(XPLMDataRef dr) {
    return dr ? XPLMGetDataf(dr) : 0.0f;
}

// ---------------------------------------------------------------------------

bool BezelLights::operator==(const BezelLights& o) const {
    return ap         == o.ap
        && fd         == o.fd
        && hdg        == o.hdg
        && nav        == o.nav
        && alt        == o.alt
        && vs         == o.vs
        && flc        == o.flc
        && apr        == o.apr
        && bc         == o.bc
        && vnv        == o.vnv
        && audio_com1 == o.audio_com1
        && audio_com2 == o.audio_com2
        && audio_nav1 == o.audio_nav1
        && audio_nav2 == o.audio_nav2
        && audio_mic1 == o.audio_mic1
        && audio_mic2 == o.audio_mic2
        && audio_spkr == o.audio_spkr
        && brightness == o.brightness;
}

// ---------------------------------------------------------------------------

BacklightManager::BacklightManager()
    : m_initialized(false), m_last_tick_time(0.0) {}

void BacklightManager::init() {
    // ---- Autopilot datarefs ----
    // flight_director_mode: 0=off, 1=on/FD, 2=engaged(AP)
    m_refs.ap_state  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/flight_director_mode");
    m_refs.fd_state  = m_refs.ap_state;  // same dataref, different threshold
    m_refs.hdg_mode  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/heading_mode");
    m_refs.nav_mode  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/nav_mode");
    m_refs.alt_hold  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/altitude_hold_mode");
    m_refs.vs_mode   = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/vvi_status");
    m_refs.flc_mode  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/airspeed_status");
    m_refs.apr_mode  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/approach_status");
    m_refs.bc_mode   = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/backcourse_status");
    m_refs.vnv_mode  = (int*)XPLMFindDataRef("sim/cockpit2/autopilot/vnav_status");

    // ---- Audio panel datarefs ----
    // audio_com_selection_pilot: bitmask — bit0=COM1, bit1=COM2
    m_refs.audio_com_sel = (int*)XPLMFindDataRef(
        "sim/cockpit2/radios/actuators/audio_com_selection_pilot");
    // audio_nav_selection_pilot: bitmask — bit0=NAV1, bit1=NAV2
    m_refs.audio_nav_sel = (int*)XPLMFindDataRef(
        "sim/cockpit2/radios/actuators/audio_nav_selection_pilot");
    // audio_selection_pilot: 1=COM1, 2=COM2 (active mic)
    m_refs.audio_mic_sel = (int*)XPLMFindDataRef(
        "sim/cockpit2/radios/actuators/audio_selection_pilot");
    m_refs.audio_spkr = (int*)XPLMFindDataRef(
        "sim/cockpit2/radios/actuators/audio_speaker_enable");

    // ---- Panel brightness ----
    m_refs.panel_bright = (float*)XPLMFindDataRef(
        "sim/cockpit2/switches/instrument_brightness_ratio[0]");

    // Warn on any missing datarefs (not fatal — we default to 0)
    if (!m_refs.ap_state)      XPLMDebugString("[X1000] BacklightManager: AP dataref not found\n");
    if (!m_refs.audio_com_sel) XPLMDebugString("[X1000] BacklightManager: audio COM dataref not found\n");
    if (!m_refs.audio_mic_sel) XPLMDebugString("[X1000] BacklightManager: audio MIC dataref not found\n");

    m_initialized = true;
    XPLMDebugString("[X1000] BacklightManager: initialised\n");
}

// ---------------------------------------------------------------------------
// Read current X-Plane state → BezelLights
// ---------------------------------------------------------------------------

BezelLights BacklightManager::readPFDLights() {
    BezelLights l;

    // AP: flight_director_mode >= 2 means autopilot engaged
    int fd_mode = readInt((XPLMDataRef)m_refs.ap_state);
    l.ap  = (fd_mode >= 2);
    l.fd  = (fd_mode >= 1);

    l.hdg = (readInt((XPLMDataRef)m_refs.hdg_mode) > 0);
    l.nav = (readInt((XPLMDataRef)m_refs.nav_mode)  > 0);
    l.alt = (readInt((XPLMDataRef)m_refs.alt_hold)  > 0);
    l.vs  = (readInt((XPLMDataRef)m_refs.vs_mode)   > 0);
    l.flc = (readInt((XPLMDataRef)m_refs.flc_mode)  > 0);
    l.apr = (readInt((XPLMDataRef)m_refs.apr_mode)  > 0);
    l.bc  = (readInt((XPLMDataRef)m_refs.bc_mode)   > 0);
    l.vnv = (readInt((XPLMDataRef)m_refs.vnv_mode)  > 0);

    // Audio panel — bitmask reads
    int com_sel = readInt((XPLMDataRef)m_refs.audio_com_sel);
    int nav_sel = readInt((XPLMDataRef)m_refs.audio_nav_sel);
    int mic_sel = readInt((XPLMDataRef)m_refs.audio_mic_sel);

    l.audio_com1 = (com_sel & 0x01) != 0;
    l.audio_com2 = (com_sel & 0x02) != 0;
    l.audio_nav1 = (nav_sel & 0x01) != 0;
    l.audio_nav2 = (nav_sel & 0x02) != 0;
    l.audio_mic1 = (mic_sel == 1);
    l.audio_mic2 = (mic_sel == 2);
    l.audio_spkr = (readInt((XPLMDataRef)m_refs.audio_spkr) != 0);

    // Panel brightness → 0-100
    float bright = readFloat((XPLMDataRef)m_refs.panel_bright);
    l.brightness = (uint8_t)(bright * 100.0f);

    return l;
}

BezelLights BacklightManager::readMFDLights() {
    // MFD has no audio panel; autopilot annunciators mirror PFD.
    BezelLights l;
    int fd_mode = readInt((XPLMDataRef)m_refs.ap_state);
    l.ap  = (fd_mode >= 2);
    l.fd  = (fd_mode >= 1);
    l.hdg = (readInt((XPLMDataRef)m_refs.hdg_mode) > 0);
    l.nav = (readInt((XPLMDataRef)m_refs.nav_mode)  > 0);
    l.alt = (readInt((XPLMDataRef)m_refs.alt_hold)  > 0);
    l.vs  = (readInt((XPLMDataRef)m_refs.vs_mode)   > 0);
    l.flc = (readInt((XPLMDataRef)m_refs.flc_mode)  > 0);
    l.apr = (readInt((XPLMDataRef)m_refs.apr_mode)  > 0);
    l.bc  = (readInt((XPLMDataRef)m_refs.bc_mode)   > 0);
    l.vnv = (readInt((XPLMDataRef)m_refs.vnv_mode)  > 0);

    float bright = readFloat((XPLMDataRef)m_refs.panel_bright);
    l.brightness = (uint8_t)(bright * 100.0f);

    return l;
}

// ---------------------------------------------------------------------------
// Serialise to wire format
// ---------------------------------------------------------------------------

std::string BacklightManager::serialise(const BezelLights& l, bool include_audio) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "ClientAv|BL_AP=%d|BL_FD=%d|BL_HDG=%d|BL_NAV=%d|BL_ALT=%d"
        "|BL_VS=%d|BL_FLC=%d|BL_APR=%d|BL_BC=%d|BL_VNV=%d|BL_BRIGHT=%d",
        l.ap  ? 1 : 0,
        l.fd  ? 1 : 0,
        l.hdg ? 1 : 0,
        l.nav ? 1 : 0,
        l.alt ? 1 : 0,
        l.vs  ? 1 : 0,
        l.flc ? 1 : 0,
        l.apr ? 1 : 0,
        l.bc  ? 1 : 0,
        l.vnv ? 1 : 0,
        l.brightness);

    if (include_audio && n < (int)sizeof(buf) - 1) {
        snprintf(buf + n, sizeof(buf) - n,
            "|BL_AUDIO_COM1=%d|BL_AUDIO_COM2=%d"
            "|BL_AUDIO_NAV1=%d|BL_AUDIO_NAV2=%d"
            "|BL_AUDIO_MIC1=%d|BL_AUDIO_MIC2=%d|BL_AUDIO_SPKR=%d",
            l.audio_com1 ? 1 : 0,
            l.audio_com2 ? 1 : 0,
            l.audio_nav1 ? 1 : 0,
            l.audio_nav2 ? 1 : 0,
            l.audio_mic1 ? 1 : 0,
            l.audio_mic2 ? 1 : 0,
            l.audio_spkr ? 1 : 0);
    }

    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Tick — called from flight loop at ~5 Hz
// ---------------------------------------------------------------------------

void BacklightManager::tick(UDPSocket& sock,
                             const std::string& pfd_ip,
                             const std::string& mfd_ip,
                             uint16_t send_port) {
    if (!m_initialized) return;

    double t = Platform::now_seconds();
    if ((t - m_last_tick_time) < TICK_INTERVAL) return;
    m_last_tick_time = t;

    // PFD
    BezelLights pfd = readPFDLights();
    if (pfd != m_last_pfd) {
        std::string msg = serialise(pfd, /*include_audio=*/true);
        sock.send(msg, pfd_ip, send_port);
        m_last_pfd = pfd;
    }

    // MFD
    BezelLights mfd = readMFDLights();
    if (mfd != m_last_mfd) {
        std::string msg = serialise(mfd, /*include_audio=*/false);
        sock.send(msg, mfd_ip, send_port);
        m_last_mfd = mfd;
    }
}
