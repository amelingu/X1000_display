// SettingsManager.cpp — Load/save X1000_display.ini and detect local IP

#include "SettingsManager.h"
#include "Platform.h"
#include <XPLMUtilities.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

SettingsManager::SettingsManager() {}

void SettingsManager::init(const std::string& plugin_dir) {
    m_plugin_dir = plugin_dir;
    if (m_settings.pc_ip.empty())
        m_settings.pc_ip = detectLocalIP();
    load();
    // Re-detect IP if not saved yet
    if (m_settings.pc_ip.empty())
        m_settings.pc_ip = detectLocalIP();
}

std::string SettingsManager::iniPath() const {
    // Store ini at plugin root (one level up from lin_x64/win_x64/mac_x64/)
    // so the same ini is shared across all platforms on the same machine.
    return Platform::normalisePath(m_plugin_dir + "/../X1000_display.ini");
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

void SettingsManager::load() {
    std::ifstream f(iniPath());
    if (!f.is_open()) {
        XPLMDebugString("[X1000] Settings: no ini file found, using defaults.\n");
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Strip comments and whitespace
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        if (line.empty() || line[0] == '[') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        trim(key); trim(val);
        if (key.empty()) continue;

        try {
            if      (key == "relay_pfd_port")  m_settings.relay_pfd_port  = (uint16_t)std::stoi(val);
            else if (key == "relay_mfd_port")  m_settings.relay_mfd_port  = (uint16_t)std::stoi(val);
            else if (key == "stream_width")    m_settings.stream_width    = std::stoi(val);
            else if (key == "jpeg_quality")    m_settings.jpeg_quality    = std::stoi(val);
            else if (key == "fps")             m_settings.fps             = std::stod(val);
            else if (key == "autostart")       m_settings.autostart       = (val == "1" || val == "true");
            else if (key == "window_x")        m_settings.window_x        = std::stoi(val);
            else if (key == "window_y")        m_settings.window_y        = std::stoi(val);
            else if (key == "window_visible")  m_settings.window_visible  = (val == "1");
            else if (key == "pc_ip")           m_settings.pc_ip           = val;
            else if (key == "pfd_bezel_mac")   m_settings.pfd_bezel_mac   = val;
            else if (key == "mfd_bezel_mac")   m_settings.mfd_bezel_mac   = val;
            else if (key == "bezel_pfd_port")  m_settings.bezel_pfd_port  = (uint16_t)std::stoi(val);
            else if (key == "bezel_mfd_port")  m_settings.bezel_mfd_port  = (uint16_t)std::stoi(val);
        } catch (...) {}
    }

    XPLMDebugString("[X1000] Settings loaded.\n");
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

void SettingsManager::save() {
    std::ofstream f(iniPath());
    if (!f.is_open()) {
        XPLMDebugString("[X1000] Settings: could not write ini file.\n");
        return;
    }

    f << "# X1000_display settings — auto-generated, safe to edit manually\n\n";
    f << "[network]\n";
    f << "relay_pfd_port=" << m_settings.relay_pfd_port << "\n";
    f << "relay_mfd_port=" << m_settings.relay_mfd_port << "\n";
    f << "pc_ip="          << m_settings.pc_ip          << "\n\n";
    f << "[bezel]\n";
    f << "pfd_bezel_mac="  << m_settings.pfd_bezel_mac  << "\n";
    f << "mfd_bezel_mac="  << m_settings.mfd_bezel_mac  << "\n";
    f << "bezel_pfd_port=" << m_settings.bezel_pfd_port << "\n";
    f << "bezel_mfd_port=" << m_settings.bezel_mfd_port << "\n\n";
    f << "[stream]\n";
    f << "stream_width="   << m_settings.stream_width   << "\n";
    f << "jpeg_quality="   << m_settings.jpeg_quality   << "\n";
    f << "fps="            << m_settings.fps            << "\n\n";
    f << "[behaviour]\n";
    f << "autostart="      << (m_settings.autostart ? 1 : 0) << "\n\n";
    f << "[ui]\n";
    f << "window_x="       << m_settings.window_x       << "\n";
    f << "window_y="       << m_settings.window_y       << "\n";
    f << "window_visible=" << (m_settings.window_visible ? 1 : 0) << "\n";

    XPLMDebugString("[X1000] Settings saved.\n");
}

// ---------------------------------------------------------------------------
// IP detection — prefer 192.168.x.x, fallback to 10.x.x.x, then any non-lo
// ---------------------------------------------------------------------------

std::string SettingsManager::detectLocalIP() {
    return Platform::detectLocalIP();
}
