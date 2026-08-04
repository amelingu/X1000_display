// BacklightManager.cpp — Reads X-Plane datarefs and pushes LED state to bezel.
//
// PROTOCOL (fully decoded via Wireshark, August 2026):
//   Each LED has an ON byte and an OFF byte: OFF = ON + 0x17
//   Brightness scale: 0x00=max bright, 0x40=off (INVERTED from intuition)
//   No reset (0x00) needed — write ON or OFF byte directly.
//
// LED ON bytes:
//   COM1_MIC=0x43  COM2_MIC=0x44  COM3_MIC=0x45  COM_1/2=0x46
//   COM1=0x47      COM2=0x48      SPKR=0x4c      MKR=0x4d
//   HI_SENS=0x4e   DME=0x4f       ADF=0x50       NAV1=0x52
//   NAV2=0x53      VOL=0x58       SQ=0x59
//
// Wire format (plugin → x1000_bezel.py, port 15684, binary):
//   Byte 0:     brightness (inverted: 0x00=max, 0x40=off)
//   Bytes 1..N: BLE bytes to write — pre-computed ON or OFF for each LED

#include "BacklightManager.h"
#include "Platform.h"
#include <XPLMDataAccess.h>
#include <XPLMUtilities.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// LED byte constants
// ---------------------------------------------------------------------------

static constexpr uint8_t LED_OFF = 0x17;  // add to ON byte to get OFF byte

static constexpr uint8_t LED_COM1_MIC = 0x43;
static constexpr uint8_t LED_COM2_MIC = 0x44;
static constexpr uint8_t LED_COM1     = 0x47;
static constexpr uint8_t LED_COM2     = 0x48;
static constexpr uint8_t LED_SPKR     = 0x4c;
static constexpr uint8_t LED_MKR      = 0x4d;
static constexpr uint8_t LED_DME      = 0x4f;
static constexpr uint8_t LED_ADF      = 0x50;
static constexpr uint8_t LED_NAV1     = 0x52;
static constexpr uint8_t LED_NAV2     = 0x53;

// ---------------------------------------------------------------------------

static int readInt(XPLMDataRef dr) {
    return dr ? XPLMGetDatai(dr) : 0;
}

bool BezelLights::operator==(const BezelLights& o) const {
    return brightness  == o.brightness
        && audio_com1  == o.audio_com1
        && audio_com2  == o.audio_com2
        && audio_nav1  == o.audio_nav1
        && audio_nav2  == o.audio_nav2
        && audio_adf1  == o.audio_adf1
        && audio_dme1  == o.audio_dme1
        && audio_mkr   == o.audio_mkr
        && audio_mic1  == o.audio_mic1
        && audio_mic2  == o.audio_mic2
        && audio_spkr  == o.audio_spkr;
}

// ---------------------------------------------------------------------------

BacklightManager::BacklightManager()
    : m_initialized(false), m_last_tick_time(0.0) {}

void BacklightManager::init() {
    auto fdr = [](const char* n) -> XPLMDataRef {
        return XPLMFindDataRef(n);
    };

    m_refs.audio_sel_com1 = fdr("sim/cockpit2/radios/actuators/audio_selection_com1");
    m_refs.audio_sel_com2 = fdr("sim/cockpit2/radios/actuators/audio_selection_com2");
    m_refs.audio_sel_nav1 = fdr("sim/cockpit2/radios/actuators/audio_selection_nav1");
    m_refs.audio_sel_nav2 = fdr("sim/cockpit2/radios/actuators/audio_selection_nav2");
    m_refs.audio_sel_adf1 = fdr("sim/cockpit2/radios/actuators/audio_selection_adf1");
    m_refs.audio_sel_dme1 = fdr("sim/cockpit2/radios/actuators/audio_dme_enabled");
    m_refs.audio_spkr     = fdr("sim/cockpit2/radios/actuators/audio_speaker_enable");
    m_refs.audio_mkr      = fdr("sim/cockpit2/radios/actuators/audio_marker_enabled");
    m_refs.audio_com_sel  = fdr("sim/cockpit2/radios/actuators/audio_com_selection");
    m_refs.panel_bright   = fdr("sim/cockpit2/electrical/panel_brightness_ratio");

    if (!m_refs.audio_sel_com1)
        XPLMDebugString("[X1000] BacklightManager: audio COM dataref not found\n");
    if (!m_refs.audio_com_sel)
        XPLMDebugString("[X1000] BacklightManager: audio MIC dataref not found\n");
    if (!m_refs.panel_bright)
        XPLMDebugString("[X1000] BacklightManager: panel brightness dataref not found\n");

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

    // Brightness: inverted scale (0x00=max bright, 0x40=off)
    // panel_brightness_ratio[3]: 0.0=off → 0x40, 1.0=max → 0x00
    float bright = 0.0f;
    if (m_refs.panel_bright)
        XPLMGetDatavf(m_refs.panel_bright, &bright, 3, 1);
    // Clamp to 1 minimum — 0x00 is a reset command, not "max brightness"
    l.brightness = (uint8_t)std::max(1.0f, (1.0f - bright) * 64.0f);

    return l;
}

BezelLights BacklightManager::readMFDLights() {
    BezelLights l;
    float bright = 0.0f;
    if (m_refs.panel_bright)
        XPLMGetDatavf(m_refs.panel_bright, &bright, 3, 1);
    // Clamp to 1 minimum — 0x00 is a reset command, not "max brightness"
    l.brightness = (uint8_t)std::max(1.0f, (1.0f - bright) * 64.0f);
    return l;
}

// ---------------------------------------------------------------------------
// Serialise — brightness byte + ON/OFF byte for each tracked LED
// The bezel script writes each byte directly to BLE, no reset needed.
// ---------------------------------------------------------------------------

static void pushLED(std::vector<uint8_t>& p, uint8_t on_byte, bool active) {
    p.push_back(active ? on_byte : (on_byte + LED_OFF));
}

static std::string serialise(const BezelLights& l, bool include_audio) {
    std::vector<uint8_t> packet;
    packet.push_back(l.brightness);

    if (include_audio) {
        pushLED(packet, LED_COM1_MIC, l.audio_mic1);
        pushLED(packet, LED_COM2_MIC, l.audio_mic2);
        pushLED(packet, LED_COM1,     l.audio_com1);
        pushLED(packet, LED_COM2,     l.audio_com2);
        pushLED(packet, LED_NAV1,     l.audio_nav1);
        pushLED(packet, LED_NAV2,     l.audio_nav2);
        pushLED(packet, LED_ADF,      l.audio_adf1);
        pushLED(packet, LED_DME,      l.audio_dme1);
        pushLED(packet, LED_MKR,      l.audio_mkr);
        pushLED(packet, LED_SPKR,     l.audio_spkr);
    }

    return std::string(reinterpret_cast<const char*>(packet.data()), packet.size());
}

// ---------------------------------------------------------------------------

void BacklightManager::tick(UDPSocket& sock,
                             const std::string& pfd_ip,
                             const std::string& mfd_ip,
                             uint16_t send_port) {
    if (!m_initialized) return;

    double t = Platform::now_seconds();
    if ((t - m_last_tick_time) < TICK_INTERVAL) return;
    m_last_tick_time = t;

    BezelLights pfd = readPFDLights();
    if (pfd != m_last_pfd) {
        std::string msg = serialise(pfd, true);
        sock.send(msg, pfd_ip, send_port);
        char dbuf[80];
        snprintf(dbuf, sizeof(dbuf),
                 "[X1000] BL: brightness=%d audio=%d%d%d%d%d%d%d%d%d%d\n",
                 (int)pfd.brightness,
                 pfd.audio_mic1, pfd.audio_mic2,
                 pfd.audio_com1, pfd.audio_com2,
                 pfd.audio_nav1, pfd.audio_nav2,
                 pfd.audio_adf1, pfd.audio_dme1,
                 pfd.audio_mkr,  pfd.audio_spkr);
        XPLMDebugString(dbuf);
        m_last_pfd = pfd;
    }

    BezelLights mfd = readMFDLights();
    if (mfd != m_last_mfd) {
        std::string msg = serialise(mfd, false);
        sock.send(msg, mfd_ip, send_port);
        m_last_mfd = mfd;
    }
}
