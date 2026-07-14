// BacklightManager.cpp — Reads X-Plane autopilot / audio datarefs and
// pushes LED state updates to x1000_bezel.py at 5 Hz.
//
// New wire format (plugin → x1000_bezel.py on port 15684, binary):
//   Byte 0:     brightness (0=off, 64=max) — maps to X-Plane panel brightness
//   Bytes 1..N: UKP release values for buttons whose LED should be ON
//
// The bezel script writes brightness then each LED byte to BLE handle 0x0008.
//
// Audio panel LED UKP release values (from Wireshark capture, July 2026):
//   COM1/MIC=43  COM2/MIC=45  COM3/MIC=47
//   COM1=51      COM2=53      COM3=55      COM1/2=49
//   NAV1=131     NAV2=133     ADF=127      DME=125     AUX=129
//   SPKR=119     MKR/MUTE=121 HI_SENS=123
//   TEL=115      PA=117       MAN_SQ=135   PLAY=137
//   PILOT=139    COPLT=141    DISPLAY_BACKUP=143

#include "BacklightManager.h"
#include "Platform.h"
#include <XPLMDataAccess.h>
#include <XPLMUtilities.h>
#include <cstdio>
#include <cstring>
#include <vector>

static int readInt(XPLMDataRef dr) {
    return dr ? XPLMGetDatai(dr) : 0;
}

static float readFloat(XPLMDataRef dr) {
    return dr ? XPLMGetDataf(dr) : 0.0f;
}

// ---------------------------------------------------------------------------

bool BezelLights::operator==(const BezelLights& o) const {
    return brightness    == o.brightness
        && audio_com1    == o.audio_com1
        && audio_com2    == o.audio_com2
        && audio_nav1    == o.audio_nav1
        && audio_nav2    == o.audio_nav2
        && audio_adf1    == o.audio_adf1
        && audio_dme1    == o.audio_dme1
        && audio_mkr     == o.audio_mkr
        && audio_mic1    == o.audio_mic1
        && audio_mic2    == o.audio_mic2
        && audio_spkr    == o.audio_spkr;
}

// ---------------------------------------------------------------------------

BacklightManager::BacklightManager()
    : m_initialized(false), m_last_tick_time(0.0) {}

void BacklightManager::init() {
    // Audio panel datarefs
    auto fdr = [](const char* n) -> XPLMDataRef {
        XPLMDataRef dr = XPLMFindDataRef(n);
        return dr;
    };
    m_refs.audio_sel_com1 = fdr("sim/cockpit2/radios/actuators/audio_selection_com1");
    m_refs.audio_sel_com2 = fdr("sim/cockpit2/radios/actuators/audio_selection_com2");
    m_refs.audio_sel_nav1 = fdr("sim/cockpit2/radios/actuators/audio_selection_nav1");
    m_refs.audio_sel_nav2 = fdr("sim/cockpit2/radios/actuators/audio_selection_nav2");
    m_refs.audio_sel_adf1 = fdr("sim/cockpit2/radios/actuators/audio_selection_adf1");
    m_refs.audio_sel_dme1 = fdr("sim/cockpit2/radios/actuators/audio_selection_dme1");
    m_refs.audio_spkr     = fdr("sim/cockpit2/radios/actuators/audio_speaker_enable");
    m_refs.audio_mkr      = fdr("sim/cockpit2/radios/actuators/audio_selection_mkr");
    m_refs.audio_com_sel  = fdr("sim/cockpit2/radios/actuators/audio_com_selection");

    // Panel brightness — 4th value of panel_brightness_ratio array (index 3)
    // In C172, this follows the main panel brightness knob.
    m_refs.panel_bright = XPLMFindDataRef(
        "sim/cockpit2/electrical/panel_brightness_ratio");
    if (!m_refs.panel_bright)
        XPLMDebugString("[X1000] BacklightManager: panel brightness dataref not found\n");

    if (!m_refs.audio_sel_com1)
        XPLMDebugString("[X1000] BacklightManager: audio COM dataref not found\n");
    if (!m_refs.audio_com_sel)
        XPLMDebugString("[X1000] BacklightManager: audio MIC dataref not found\n");

    m_initialized = true;
    XPLMDebugString("[X1000] BacklightManager: initialised\n");
}

// ---------------------------------------------------------------------------

BezelLights BacklightManager::readPFDLights() {
    BezelLights l;

    l.audio_com1 = (readInt(m_refs.audio_sel_com1) != 0);
    l.audio_com2 = (readInt(m_refs.audio_sel_com2) != 0);
    l.audio_nav1 = (readInt(m_refs.audio_sel_nav1) != 0);
    l.audio_nav2 = (readInt(m_refs.audio_sel_nav2) != 0);
    l.audio_adf1 = (readInt(m_refs.audio_sel_adf1) != 0);
    l.audio_dme1 = (readInt(m_refs.audio_sel_dme1) != 0);
    l.audio_mkr  = (readInt(m_refs.audio_mkr)      != 0);
    l.audio_spkr = (readInt(m_refs.audio_spkr)     != 0);

    int mic_sel  = readInt(m_refs.audio_com_sel);
    l.audio_mic1 = (mic_sel == 6);
    l.audio_mic2 = (mic_sel == 7);

    // Read 4th value (index 3) of panel_brightness_ratio array → 0-64 bezel scale
    float bright = 0.0f;
    if (m_refs.panel_bright) {
        XPLMGetDatavf(m_refs.panel_bright, &bright, 3, 1);
    }
    l.brightness = (uint8_t)(bright * 64.0f);

    return l;
}

BezelLights BacklightManager::readMFDLights() {
    // MFD has no audio panel — just brightness
    BezelLights l;
    float bright = 0.0f;
    if (m_refs.panel_bright) {
        XPLMGetDatavf(m_refs.panel_bright, &bright, 3, 1);
    }
    l.brightness = (uint8_t)(bright * 64.0f);
    return l;
}

// ---------------------------------------------------------------------------
// Serialise to binary LED state packet
// Format: 1 byte brightness + N bytes of active LED UKP values
// ---------------------------------------------------------------------------

static std::string serialiseBinary(const BezelLights& l, bool include_audio) {
    std::vector<uint8_t> packet;
    packet.push_back(l.brightness);

    if (include_audio) {
        // Audio panel button LED UKP release values
        if (l.audio_mic1) packet.push_back(43);   // COM1/MIC
        if (l.audio_mic2) packet.push_back(45);   // COM2/MIC
        if (l.audio_com1) packet.push_back(51);   // COM1 monitor
        if (l.audio_com2) packet.push_back(53);   // COM2 monitor
        if (l.audio_nav1) packet.push_back(131);  // NAV1
        if (l.audio_nav2) packet.push_back(133);  // NAV2
        if (l.audio_adf1) packet.push_back(127);  // ADF
        if (l.audio_dme1) packet.push_back(125);  // DME
        if (l.audio_mkr)  packet.push_back(121);  // MKR/MUTE
        if (l.audio_spkr) packet.push_back(119);  // SPKR
    }

    return std::string(reinterpret_cast<const char*>(packet.data()), packet.size());
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

    // PFD (includes audio panel LEDs)
    BezelLights pfd = readPFDLights();
    if (pfd != m_last_pfd) {
        std::string msg = serialiseBinary(pfd, /*include_audio=*/true);
        sock.send(msg, pfd_ip, send_port);
        // Debug: log what we sent
        char dbuf[128];
        snprintf(dbuf, sizeof(dbuf),
                 "[X1000] BL: PFD brightness=%d leds=%zu\n",
                 (int)pfd.brightness, msg.size() - 1);
        XPLMDebugString(dbuf);
        m_last_pfd = pfd;
    }

    // MFD (brightness only)
    BezelLights mfd = readMFDLights();
    if (mfd != m_last_mfd) {
        std::string msg = serialiseBinary(mfd, /*include_audio=*/false);
        sock.send(msg, mfd_ip, send_port);
        m_last_mfd = mfd;
    }
}
