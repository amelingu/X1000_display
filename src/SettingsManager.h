#pragma once
// SettingsManager.h — Persistent settings stored in X1000_display.ini
// alongside the .xpl file. Loaded at plugin start, saved on any change.

#include <string>
#include <cstdint>

struct Settings {
    // Network
    uint16_t relay_pfd_port = 9000;
    uint16_t relay_mfd_port = 9001;

    // Stream quality
    int    stream_width   = 1024;
    int    jpeg_quality   = 85;
    double fps            = 15.0;

    // Behaviour
    bool   autostart      = true;  // start streaming when G1000 detected

    // UI window position
    int    window_x       = 50;
    int    window_y       = 50;
    bool   window_visible = false;

    // Detected/configured PC IP (shown in setup guide)
    std::string pc_ip = "";

    // Bezel Bluetooth MAC addresses
    std::string pfd_bezel_mac = "";  // e.g. "00:07:80:A6:E1:71"
    std::string mfd_bezel_mac = "";  // e.g. "00:07:80:A6:F5:0A"

    // Bezel UDP ports
    uint16_t bezel_pfd_port = 15683;
    uint16_t bezel_mfd_port = 15685;
};

class SettingsManager {
public:
    SettingsManager();

    // Call once at plugin start — locates ini file next to .xpl
    void init(const std::string& plugin_dir);

    void load();
    void save();

    Settings& get()             { return m_settings; }
    const Settings& get() const { return m_settings; }

    // Detect best LAN IP for the setup guide
    static std::string detectLocalIP();

private:
    std::string iniPath() const;
    std::string m_plugin_dir;
    Settings    m_settings;
};
