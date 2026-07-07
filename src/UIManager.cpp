// UIManager.cpp — In-sim settings window using XPLMDisplay modern window API

#include "UIManager.h"
#include "Platform.h"
#include <XPLMUtilities.h>
#include <XPLMGraphics.h>
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <GL/gl.h>

// ---------------------------------------------------------------------------
// Hit-test registry — populated during draw(), consumed by handleClick()
// ---------------------------------------------------------------------------

enum class ClickAction {
    NONE,
    RELAY_STARTSTOP,
    RELAY_SHOWLOG,
    RELAY_CLEARLOG,
    REDETECT_IP,
    APPLY,
    FIELD_PFD_PORT,
    FIELD_MFD_PORT,
    FIELD_WIDTH,
    FIELD_QUALITY,
    FIELD_FPS,
    FIELD_PC_IP,
    CHECKBOX_AUTOSTART,
    FIELD_PFD_MAC,
    FIELD_MFD_MAC,
    BEZEL_SCAN,
    BEZEL_ASSIGN_PFD,
    BEZEL_ASSIGN_MFD,
};

struct HitRect {
    int x1, y1, x2, y2;
    ClickAction action;
    int field_id; // for FIELD_* actions
};

static std::vector<HitRect> s_hit_rects;

static void regHit(int x, int y, int w, int h, ClickAction a, int fid = -1) {
    s_hit_rects.push_back({x, y, x+w, y+h, a, fid});
}

// ---------------------------------------------------------------------------

UIManager::UIManager() {}

UIManager::~UIManager() {
    if (m_window) { XPLMDestroyWindow(m_window); m_window = nullptr; }
    if (m_menu)   { XPLMDestroyMenu(m_menu);     m_menu   = nullptr; }
    if (m_menu_item >= 0) { XPLMRemoveMenuItem(XPLMFindPluginsMenu(), m_menu_item); m_menu_item = -1; }
}

// ---------------------------------------------------------------------------

void UIManager::init(SettingsManager* settings,
                     RelayManager*    relay,
                     RelayManager*    bezel_relay,
                     ApplyCallback    on_apply) {
    m_settings     = settings;
    m_relay        = relay;
    m_bezel_relay  = bezel_relay;
    m_on_apply     = on_apply;

    const Settings& s = settings->get();
    snprintf(m_buf_pfd_port, sizeof(m_buf_pfd_port), "%u",   s.relay_pfd_port);
    snprintf(m_buf_mfd_port, sizeof(m_buf_mfd_port), "%u",   s.relay_mfd_port);
    snprintf(m_buf_width,    sizeof(m_buf_width),    "%d",   s.stream_width);
    snprintf(m_buf_quality,  sizeof(m_buf_quality),  "%d",   s.jpeg_quality);
    snprintf(m_buf_fps,      sizeof(m_buf_fps),      "%.0f", s.fps);
    snprintf(m_buf_pc_ip,    sizeof(m_buf_pc_ip),    "%s",   s.pc_ip.c_str());
    snprintf(m_buf_pfd_mac,  sizeof(m_buf_pfd_mac),  "%s",   s.pfd_bezel_mac.c_str());
    snprintf(m_buf_mfd_mac,  sizeof(m_buf_mfd_mac),  "%s",   s.mfd_bezel_mac.c_str());

    int screen_w, screen_h;
    XPLMGetScreenSize(&screen_w, &screen_h);
    int wx  = s.window_x;
    int wy  = screen_h - s.window_y;
    int wx2 = wx  + WIN_W;
    int wy2 = wy  - WIN_H;

    XPLMCreateWindow_t p = {};
    p.structSize              = sizeof(p);
    p.left                    = wx;
    p.top                     = wy;
    p.right                   = wx2;
    p.bottom                  = wy2;
    p.visible                 = s.window_visible ? 1 : 0;
    p.drawWindowFunc          = drawCB;
    p.handleMouseClickFunc    = mouseCB;
    p.handleKeyFunc           = keyboardCB;
    p.handleCursorFunc        = cursorCB;
    p.refcon                  = this;
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    p.layer                   = xplm_WindowLayerFloatingWindows;

    m_window = XPLMCreateWindowEx(&p);
    XPLMSetWindowTitle(m_window, "X1000 Display");
    XPLMSetWindowResizingLimits(m_window, WIN_W, WIN_H, WIN_W, WIN_H);
    XPLMSetWindowPositioningMode(m_window, xplm_WindowPositionFree, -1);

    m_menu_item = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "X1000 Display", nullptr, 1);
    m_menu = XPLMCreateMenu("X1000 Display", XPLMFindPluginsMenu(),
                            m_menu_item, menuCB, this);
    XPLMAppendMenuItem(m_menu, "Settings", (void*)this, 1);
}

// ---------------------------------------------------------------------------

void UIManager::show()   { if (m_window) XPLMSetWindowIsVisible(m_window, 1); m_settings->get().window_visible = true; }
void UIManager::hide()   { if (m_window) XPLMSetWindowIsVisible(m_window, 0); m_settings->get().window_visible = false; }
void UIManager::toggle() { if (isVisible()) hide(); else show(); }
bool UIManager::isVisible() const { return m_window && XPLMGetWindowIsVisible(m_window); }

void UIManager::tick(bool pfd_s, bool mfd_s, int pf, int mf, int pk, int mk) {
    m_pfd_streaming = pfd_s; m_mfd_streaming = mfd_s;
    m_pfd_fps = pf; m_mfd_fps = mf;
    m_pfd_kb  = pk; m_mfd_kb  = mk;
    if (m_scan_state == ScanState::SCANNING) pollBezelScan();
}

void UIManager::menuCB(void*, void* item_ref) {
    auto* ui = (UIManager*)item_ref;
    if (ui) ui->toggle();
}

// ---------------------------------------------------------------------------
// XPLM callbacks
// ---------------------------------------------------------------------------

void UIManager::drawCB(XPLMWindowID, void* ref)  { ((UIManager*)ref)->draw(); }

int UIManager::mouseCB(XPLMWindowID, int x, int y, XPLMMouseStatus status, void* ref) {
    if (status == xplm_MouseDown) ((UIManager*)ref)->handleClick(x, y);
    return 1;
}

void UIManager::keyboardCB(XPLMWindowID, char key, XPLMKeyFlags,
                           char, void* ref, int) {
    auto* ui = (UIManager*)ref;
    if (ui->m_active_field < 0) return;
    char* bufs[] = { ui->m_buf_pfd_port, ui->m_buf_mfd_port,
                     ui->m_buf_width,    ui->m_buf_quality,
                     ui->m_buf_fps,      ui->m_buf_pc_ip,
                     ui->m_buf_pfd_mac,  ui->m_buf_mfd_mac };
    int   szs[]  = { 8, 8, 8, 8, 8, 32, 20, 20 };
    char* buf = bufs[ui->m_active_field];
    int   sz  = szs[ui->m_active_field];
    if (key == 8 || key == 127) {
        int len = (int)strlen(buf); if (len > 0) buf[len-1] = '\0';
    } else if (key >= 32 && key < 127) {
        int len = (int)strlen(buf);
        if (len < sz-1) { buf[len] = key; buf[len+1] = '\0'; }
    }
}

XPLMCursorStatus UIManager::cursorCB(XPLMWindowID, int, int, void*) {
    return xplm_CursorDefault;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void UIManager::drawText(int x, int y, const char* text, float r, float g, float b) {
    float col[3] = {r, g, b};
    XPLMDrawString(col, x, y, const_cast<char*>(text), nullptr, xplmFont_Proportional);
}

void UIManager::drawRect(int x1, int y1, int x2, int y2,
                         float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2i(x1,y1); glVertex2i(x2,y1);
    glVertex2i(x2,y2); glVertex2i(x1,y2);
    glEnd();
}

// Draw a button and register its hit rect
void UIManager::drawButton(int x, int y, int w, int h, const char* label,
                           ClickAction action,
                           float r, float g, float b) {
    drawRect(x, y, x+w, y+h, r, g, b, 0.9f);
    glColor4f(0.5f,0.5f,0.5f,1);
    glBegin(GL_LINE_LOOP);
    glVertex2i(x,y); glVertex2i(x+w,y);
    glVertex2i(x+w,y+h); glVertex2i(x,y+h);
    glEnd();
    drawText(x+6, y+4, label);
    regHit(x, y, w, h, action);
}

void UIManager::drawTextInput(int x, int y, int w,
                              char* buf, int bufsz, bool active,
                              ClickAction action) {
    drawRect(x, y, x+w, y+18,
             active ? 0.18f : 0.13f,
             active ? 0.18f : 0.13f,
             active ? 0.28f : 0.13f, 1.0f);
    glColor4f(active ? 0.5f : 0.3f, active ? 0.5f : 0.3f,
              active ? 0.9f : 0.3f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2i(x,y); glVertex2i(x+w,y);
    glVertex2i(x+w,y+18); glVertex2i(x,y+18);
    glEnd();
    drawText(x+4, y+3, buf);
    if (active) {
        int cx = x + 4 + (int)(strlen(buf) * 7);
        glColor4f(1,1,1,1);
        glBegin(GL_LINES);
        glVertex2i(cx,y+2); glVertex2i(cx,y+15);
        glEnd();
    }
    regHit(x, y, w, 18, action);
    (void)bufsz;
}

void UIManager::drawCheckbox(int x, int y, const char* label, bool& value,
                             ClickAction action) {
    drawRect(x, y, x+14, y+14,
             value ? 0.1f  : 0.13f,
             value ? 0.45f : 0.13f,
             value ? 0.1f  : 0.13f, 1.0f);
    glColor4f(0.5f,0.5f,0.5f,1);
    glBegin(GL_LINE_LOOP);
    glVertex2i(x,y); glVertex2i(x+14,y);
    glVertex2i(x+14,y+14); glVertex2i(x,y+14);
    glEnd();
    if (value) drawText(x+2, y+1, "x", 0.2f, 1.0f, 0.2f);
    drawText(x+20, y+1, label);
    regHit(x, y, (int)strlen(label)*8 + 24, 14, action);
}

void UIManager::drawSection(const char* title, int& y, int left, int right) {
    y -= 6;
    drawRect(left+4, y-1, right-4, y+14, 0.13f, 0.16f, 0.23f, 1.0f);
    drawText(left+8, y+1, title, 0.55f, 0.75f, 1.0f);
    y -= 18;
}

// ---------------------------------------------------------------------------
// Main draw — rebuilds hit rects every frame
// ---------------------------------------------------------------------------

void UIManager::draw() {
    s_hit_rects.clear();

    int left, top, right, bottom;
    XPLMGetWindowGeometry(m_window, &left, &top, &right, &bottom);

    drawRect(left, bottom, right, top, 0.10f, 0.10f, 0.13f, 0.96f);

    int y = top - 8;
    drawStatus(y, left, right);
    drawNetwork(y, left, right);
    drawSetupGuide(y, left, right);
    if (m_show_log) drawRelayLog(y, left, right);
    drawAbout(y, left, right);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void UIManager::drawStatus(int& y, int left, int right) {
    drawSection("STATUS", y, left, right);
    int x = left + 10;

    bool relay_ok = m_relay && m_relay->isRunning();
    char rbuf[48];
    snprintf(rbuf, sizeof(rbuf), "Relay:   %s", relay_ok ? "running" : "stopped");
    drawText(x, y, rbuf, relay_ok ? 0.2f : 1.0f, relay_ok ? 1.0f : 0.3f, 0.2f);
    y -= 16;

    bool bezel_ok = m_bezel_relay && m_bezel_relay->isRunning();
    char bbuf[48];
    snprintf(bbuf, sizeof(bbuf), "Bezel:   %s", bezel_ok ? "connected" : "stopped");
    drawText(x, y, bbuf, bezel_ok ? 0.2f : 0.8f, bezel_ok ? 1.0f : 0.8f, 0.2f);
    y -= 16;

    char pbuf[72];
    snprintf(pbuf, sizeof(pbuf), "PFD:     %s   %d fps   %d KB/frame",
             m_pfd_streaming ? "streaming" : "idle", m_pfd_fps, m_pfd_kb);
    drawText(x, y, pbuf, m_pfd_streaming ? 0.2f : 0.8f, m_pfd_streaming ? 1.0f : 0.8f, 0.2f);
    y -= 16;

    char mbuf[72];
    snprintf(mbuf, sizeof(mbuf), "MFD:     %s   %d fps   %d KB/frame",
             m_mfd_streaming ? "streaming" : "idle", m_mfd_fps, m_mfd_kb);
    drawText(x, y, mbuf, m_mfd_streaming ? 0.2f : 0.8f, m_mfd_streaming ? 1.0f : 0.8f, 0.2f);
    y -= 20;

    drawButton(x,     y-18, 90, 18,
               relay_ok ? "Stop Relay" : "Start Relay",
               ClickAction::RELAY_STARTSTOP,
               relay_ok ? 0.35f : 0.15f, relay_ok ? 0.1f : 0.30f, 0.1f);
    drawButton(x+100, y-18, 80, 18, m_show_log ? "Hide Log" : "Show Log",
               ClickAction::RELAY_SHOWLOG, 0.18f, 0.18f, 0.30f);
    drawButton(x+190, y-18, 80, 18, "Clear Log",
               ClickAction::RELAY_CLEARLOG, 0.22f, 0.12f, 0.12f);
    y -= 26;
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

void UIManager::drawNetwork(int& y, int left, int right) {
    drawSection("NETWORK & STREAM", y, left, right);
    int x = left + 10;

    // PC IP
    drawText(x, y+2, "PC IP:");
    drawTextInput(x+68, y, 150, m_buf_pc_ip, sizeof(m_buf_pc_ip),
                  m_active_field == 5, ClickAction::FIELD_PC_IP);
    drawButton(x+228, y, 90, 18, "Re-detect",
               ClickAction::REDETECT_IP, 0.18f, 0.18f, 0.28f);
    y -= 24;

    // Ports
    drawText(x, y+2, "PFD port:");
    drawTextInput(x+72, y, 55, m_buf_pfd_port, sizeof(m_buf_pfd_port),
                  m_active_field == 0, ClickAction::FIELD_PFD_PORT);
    drawText(x+140, y+2, "MFD port:");
    drawTextInput(x+212, y, 55, m_buf_mfd_port, sizeof(m_buf_mfd_port),
                  m_active_field == 1, ClickAction::FIELD_MFD_PORT);
    y -= 24;

    // Quality
    drawText(x, y+2, "Width px:");
    drawTextInput(x+72, y, 55, m_buf_width, sizeof(m_buf_width),
                  m_active_field == 2, ClickAction::FIELD_WIDTH);
    drawText(x+140, y+2, "Quality:");
    drawTextInput(x+212, y, 55, m_buf_quality, sizeof(m_buf_quality),
                  m_active_field == 3, ClickAction::FIELD_QUALITY);
    y -= 24;

    // FPS
    drawText(x, y+2, "FPS:");
    drawTextInput(x+72, y, 55, m_buf_fps, sizeof(m_buf_fps),
                  m_active_field == 4, ClickAction::FIELD_FPS);
    y -= 24;

    // Bezel MAC addresses
    drawText(x, y+2, "PFD bezel:");
    drawTextInput(x+80, y, 130, m_buf_pfd_mac, sizeof(m_buf_pfd_mac),
                  m_active_field == 6, ClickAction::FIELD_PFD_MAC);
    y -= 24;
    drawText(x, y+2, "MFD bezel:");
    drawTextInput(x+80, y, 130, m_buf_mfd_mac, sizeof(m_buf_mfd_mac),
                  m_active_field == 7, ClickAction::FIELD_MFD_MAC);
    y -= 24;

    // Bezel scan
    drawButton(x, y-18, 80, 18, "Scan BLE",
               ClickAction::BEZEL_SCAN, 0.15f, 0.25f, 0.35f);
    if (m_scan_state == ScanState::SCANNING) {
        drawText(x+90, y-14, "Scanning... press 3 buttons on PFD bezel!", 0.8f, 0.8f, 0.2f);
    } else if (m_scan_state == ScanState::DONE) {
        if (m_scan_found.empty()) {
            drawText(x+90, y-14, "No bezels found.", 1.0f, 0.4f, 0.4f);
        } else {
            int dy = y - 14;
            for (int i = 0; i < (int)m_scan_found.size() && i < 4; ++i) {
                char lb[64];
                snprintf(lb, sizeof(lb), "%d: %s", i+1, m_scan_found[i].c_str());
                drawText(x+90, dy, lb, 0.2f, 1.0f, 0.2f);
                // Highlight if this is the auto-detected PFD
                bool is_pfd = (m_scan_found[i] == m_settings->get().pfd_bezel_mac);
                bool is_mfd = (m_scan_found[i] == m_settings->get().mfd_bezel_mac);
                if (is_pfd) drawText(x+88+205, dy, " ← PFD", 0.4f, 1.0f, 0.4f);
                if (is_mfd) drawText(x+88+205, dy, " ← MFD", 0.4f, 0.8f, 1.0f);
                drawButton(x+295, dy-4, 45, 16, "PFD",
                           ClickAction::BEZEL_ASSIGN_PFD, 0.1f, 0.3f, 0.1f);
                s_hit_rects.back().field_id = i;
                drawButton(x+345, dy-4, 45, 16, "MFD",
                           ClickAction::BEZEL_ASSIGN_MFD, 0.1f, 0.1f, 0.3f);
                s_hit_rects.back().field_id = i;
                dy -= 20;
            }
        }
    }
    y -= 28;

    // Autostart checkbox
    Settings& s = m_settings->get();
    drawCheckbox(x, y, "Auto-start when G1000 detected", s.autostart,
                 ClickAction::CHECKBOX_AUTOSTART);
    y -= 22;

    // Apply button
    drawButton(x, y-18, 150, 18, "Apply & Restart Stream",
               ClickAction::APPLY, 0.12f, 0.30f, 0.12f);
    y -= 26;
}

// ---------------------------------------------------------------------------
// Setup guide
// ---------------------------------------------------------------------------

void UIManager::drawSetupGuide(int& y, int left, int right) {
    drawSection("IPAD SETUP GUIDE", y, left, right);
    int x = left + 10;
    const Settings& s = m_settings->get();

    char pfd_url[64], mfd_url[64];
    snprintf(pfd_url, sizeof(pfd_url), "  http://%s:8080/", s.pc_ip.c_str());
    snprintf(mfd_url, sizeof(mfd_url), "  http://%s:8081/", s.pc_ip.c_str());

    struct { const char* text; float r,g,b; } lines[] = {
        {"1. Relay starts automatically with the plugin",   0.8f,0.8f,0.8f},
        {"2. PFD iPad — open Safari:",                      0.8f,0.8f,0.8f},
        {pfd_url,                                           0.4f,0.8f,1.0f},
        {"3. MFD iPad — open Safari:",                      0.8f,0.8f,0.8f},
        {mfd_url,                                           0.4f,0.8f,1.0f},
        {"4. Safari: Share -> Add to Home Screen",          0.8f,0.8f,0.8f},
        {"5. Launch from Home Screen icon",                 0.8f,0.8f,0.8f},
        {"6. Settings -> Display -> Auto-Lock -> Never",    0.8f,0.8f,0.8f},
    };
    for (auto& l : lines) {
        drawText(x, y, l.text, l.r, l.g, l.b);
        y -= 15;
    }
    y -= 4;
    (void)right;
}

// ---------------------------------------------------------------------------
// Relay log
// ---------------------------------------------------------------------------

void UIManager::drawRelayLog(int& y, int left, int right) {
    drawSection("RELAY LOG", y, left, right);
    int x = left + 10;
    if (!m_relay) return;
    auto log = m_relay->getLog();
    int show = 6;
    int start = (int)log.size() - show - m_log_scroll;
    if (start < 0) start = 0;
    for (int i = start; i < (int)log.size() && i < start + show; ++i) {
        std::string t = log[i].text;
        if (t.size() > 62) t = t.substr(0,59) + "...";
        float r = log[i].is_error ? 1.0f : 0.72f;
        float g = log[i].is_error ? 0.3f : 0.72f;
        float b = log[i].is_error ? 0.3f : 0.72f;
        drawText(x, y, t.c_str(), r, g, b);
        y -= 14;
    }
    y -= 4;
    (void)right;
}

// ---------------------------------------------------------------------------
// About
// ---------------------------------------------------------------------------

void UIManager::drawAbout(int& y, int left, int right) {
    drawSection("ABOUT", y, left, right);
    int x = left + 10;
    drawText(x, y, "X1000 Display v1.0  --  X-Plane 12 G1000 mirror for iPad",
             0.55f, 0.55f, 0.55f);
    y -= 15;
    drawText(x, y, "MIT License  |  Built with X-Plane SDK 4.1",
             0.38f, 0.38f, 0.38f);
    y -= 15;
    (void)right;
}

// ---------------------------------------------------------------------------
// Bezel scan
// ---------------------------------------------------------------------------

void UIManager::notifyScanActivity(const std::string& mac) {
    m_scan_active_mac  = mac;
    m_scan_active_time = Platform::now_seconds();
}

void UIManager::startBezelScan() {
    m_scan_state   = ScanState::SCANNING;
    m_scan_found.clear();
    m_scan_pfd_mac.clear();

    if (!m_bezel_relay) return;

    // Stop current bezel relay, restart with --scan flag
    m_bezel_relay->stop();
    m_bezel_relay->clearLog();
    m_bezel_relay->restart_scan();

    XPLMDebugString("[X1000] UI: Bezel scan started\n");
}

void UIManager::pollBezelScan() {
    if (m_scan_state != ScanState::SCANNING || !m_bezel_relay) return;

    auto log = m_bezel_relay->getLog();
    bool done = false;

    // Debug — log all entries once
    static int s_last_log_size = 0;
    if ((int)log.size() > s_last_log_size) {
        for (int i = s_last_log_size; i < (int)log.size(); ++i) {
            std::string msg = "[X1000] SCAN-LOG: " + log[i].text + "\n";
            XPLMDebugString(msg.c_str());
        }
        s_last_log_size = (int)log.size();
    }

    for (const auto& entry : log) {
        auto pos = entry.text.find("BEZEL_FOUND:");
        if (pos != std::string::npos) {
            std::string rest = entry.text.substr(pos + 12);
            // Format: MAC:NAME where MAC itself contains colons
            // MAC is 17 chars (XX:XX:XX:XX:XX:XX), so split at position 17
            if (rest.size() >= 17) {
                std::string mac = rest.substr(0, 17);
                if (mac.size() == 17 && mac[2] == ':') {  // basic MAC validation
                    bool dup = false;
                    for (const auto& m : m_scan_found) if (m == mac) { dup = true; break; }
                    if (!dup) m_scan_found.push_back(mac);
                }
            }
        }
        // BEZEL_PFD:MAC — user pressed a button on this bezel during scan
        auto pos2 = entry.text.find("BEZEL_PFD:");
        if (pos2 != std::string::npos) {
            std::string rest = entry.text.substr(pos2 + 10);
            if (rest.size() >= 17 && rest[2] == ':') {
                m_scan_pfd_mac = rest.substr(0, 17);
            }
        }
        if (entry.text.find("BEZEL_SCAN_DONE") != std::string::npos) {
            done = true;
        }
    }

    if (done) {
        m_scan_state = ScanState::DONE;
        char dbuf[128];
        snprintf(dbuf, sizeof(dbuf), "[X1000] UI: Bezel scan complete — found %zu bezels\n",
                 m_scan_found.size());
        XPLMDebugString(dbuf);
        for (const auto& mac : m_scan_found) {
            std::string msg = "[X1000] UI: Found bezel MAC: " + mac + "\n";
            XPLMDebugString(msg.c_str());
        }
        // Auto-assign if user pressed a button on the PFD bezel during scan
        if (!m_scan_pfd_mac.empty() && m_scan_found.size() >= 1) {
            strncpy(m_buf_pfd_mac, m_scan_pfd_mac.c_str(), sizeof(m_buf_pfd_mac)-1);
            m_settings->get().pfd_bezel_mac = m_scan_pfd_mac;
            XPLMDebugString("[X1000] UI: PFD bezel auto-assigned from scan\n");
            std::string mfd_mac;
            // If only 2 bezels found, auto-assign the other as MFD
            if (m_scan_found.size() == 2) {
                mfd_mac = (m_scan_found[0] == m_scan_pfd_mac)
                           ? m_scan_found[1] : m_scan_found[0];
                strncpy(m_buf_mfd_mac, mfd_mac.c_str(), sizeof(m_buf_mfd_mac)-1);
                m_settings->get().mfd_bezel_mac = mfd_mac;
                XPLMDebugString("[X1000] UI: MFD bezel auto-assigned from scan\n");
            }
            m_settings->save();

            // Restart bezel relay with new MACs — no need to stop/restart plugin
            if (m_bezel_relay && m_on_apply) {
                XPLMDebugString("[X1000] UI: Restarting bezel relay with new MACs\n");
                m_on_apply();  // triggers applySettings in Plugin.cpp which restarts relay
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Click handling — uses s_hit_rects registered during draw()
// ---------------------------------------------------------------------------

void UIManager::handleClick(int mx, int my) {
    for (const auto& hr : s_hit_rects) {
        if (mx < hr.x1 || mx > hr.x2 || my < hr.y1 || my > hr.y2) continue;

        switch (hr.action) {

        case ClickAction::RELAY_STARTSTOP:
            if (m_relay->isRunning()) m_relay->stop();
            else m_relay->start();
            break;

        case ClickAction::RELAY_SHOWLOG:
            m_show_log = !m_show_log;
            break;

        case ClickAction::RELAY_CLEARLOG:
            if (m_relay) m_relay->clearLog();
            break;

        case ClickAction::REDETECT_IP: {
            std::string ip = SettingsManager::detectLocalIP();
            strncpy(m_buf_pc_ip, ip.c_str(), sizeof(m_buf_pc_ip)-1);
            m_settings->get().pc_ip = ip;
            m_settings->save();
            break;
        }

        case ClickAction::APPLY: {
            Settings& s = m_settings->get();
            s.relay_pfd_port = (uint16_t)atoi(m_buf_pfd_port);
            s.relay_mfd_port = (uint16_t)atoi(m_buf_mfd_port);
            s.stream_width   = atoi(m_buf_width);
            s.jpeg_quality   = atoi(m_buf_quality);
            s.fps            = atof(m_buf_fps);
            s.pc_ip          = m_buf_pc_ip;
            s.pfd_bezel_mac  = m_buf_pfd_mac;
            s.mfd_bezel_mac  = m_buf_mfd_mac;
            m_settings->save();
            if (m_on_apply) m_on_apply();
            m_active_field = -1;
            break;
        }

        case ClickAction::FIELD_PFD_PORT: m_active_field = 0; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_MFD_PORT: m_active_field = 1; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_WIDTH:    m_active_field = 2; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_QUALITY:  m_active_field = 3; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_FPS:      m_active_field = 4; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_PC_IP:    m_active_field = 5; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_PFD_MAC:  m_active_field = 6; XPLMTakeKeyboardFocus(m_window); break;
        case ClickAction::FIELD_MFD_MAC:  m_active_field = 7; XPLMTakeKeyboardFocus(m_window); break;

        case ClickAction::BEZEL_SCAN:
            if (m_scan_state != ScanState::SCANNING)
                startBezelScan();
            break;

        case ClickAction::BEZEL_ASSIGN_PFD:
            if (hr.field_id >= 0 && hr.field_id < (int)m_scan_found.size()) {
                const std::string& mac = m_scan_found[hr.field_id];
                strncpy(m_buf_pfd_mac, mac.c_str(), sizeof(m_buf_pfd_mac)-1);
                m_settings->get().pfd_bezel_mac = mac;
                m_settings->save();
            }
            break;

        case ClickAction::BEZEL_ASSIGN_MFD:
            if (hr.field_id >= 0 && hr.field_id < (int)m_scan_found.size()) {
                const std::string& mac = m_scan_found[hr.field_id];
                strncpy(m_buf_mfd_mac, mac.c_str(), sizeof(m_buf_mfd_mac)-1);
                m_settings->get().mfd_bezel_mac = mac;
                m_settings->save();
            }
            break;

        case ClickAction::CHECKBOX_AUTOSTART:
            m_settings->get().autostart = !m_settings->get().autostart;
            m_settings->save();
            break;

        default: break;
        }
        return; // only handle first match
    }
    // Click outside all fields — lose focus
    m_active_field = -1;
    XPLMTakeKeyboardFocus(nullptr);
}
