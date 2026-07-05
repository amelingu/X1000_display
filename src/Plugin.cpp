// Plugin.cpp — X1000_display entry point

#include "Platform.h"
#include "ConnectionManager.h"
#include "DisplayStreamer.h"
#include "SettingsManager.h"
#include "RelayManager.h"
#include "UIManager.h"

#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <cstring>
#include <string>

static ConnectionManager* g_conn        = nullptr;
static DisplayStreamer*   g_display     = nullptr;
static SettingsManager*   g_settings    = nullptr;
static RelayManager*      g_relay       = nullptr;   // display relay
static RelayManager*      g_bezel_relay = nullptr;   // bezel BLE bridge
static UIManager*         g_ui          = nullptr;

static std::string g_plugin_dir;

// Live stats for UI
static int  s_pfd_frame_count = 0, s_mfd_frame_count = 0;
static int  s_pfd_fps = 0,         s_mfd_fps = 0;
static int  s_pfd_kb  = 0,         s_mfd_kb  = 0;

// Auto-retry display init until G1000 is bound
static bool s_display_init_pending = false;
static double s_last_retry_time    = 0.0;

// ---------------------------------------------------------------------------

static void applySettings() {
    const Settings& s = g_settings->get();

    // Restart relay with new ports
    if (g_relay) {
        g_relay->restart(s.relay_pfd_port, s.relay_mfd_port);
    }

    // Reinit display streamer with new quality settings
    if (g_display) {
        g_display->shutdown();
        delete g_display;
    }
    g_display = new DisplayStreamer();
    StreamConfig cfg;
    cfg.pfd_ip        = "127.0.0.1";
    cfg.mfd_ip        = "127.0.0.1";
    cfg.pfd_push_port = s.relay_pfd_port;
    cfg.mfd_push_port = s.relay_mfd_port;
    cfg.stream_width  = s.stream_width;
    cfg.jpeg_quality  = s.jpeg_quality;
    cfg.fps           = s.fps;

    if (!g_display->init(cfg)) {
        XPLMDebugString("[X1000] DisplayStreamer re-init failed\n");
        delete g_display;
        g_display = nullptr;
    }
}

static void initDisplay() {
    if (g_display) { g_display->shutdown(); delete g_display; }
    g_display = new DisplayStreamer();

    const Settings& s = g_settings->get();
    StreamConfig cfg;
    cfg.pfd_ip        = "127.0.0.1";
    cfg.mfd_ip        = "127.0.0.1";
    cfg.pfd_push_port = s.relay_pfd_port;
    cfg.mfd_push_port = s.relay_mfd_port;
    cfg.stream_width  = s.stream_width;
    cfg.jpeg_quality  = s.jpeg_quality;
    cfg.fps           = s.fps;

    if (!g_display->init(cfg)) {
        XPLMDebugString("[X1000] DisplayStreamer init failed — will retry\n");
        delete g_display;
        g_display = nullptr;
        s_display_init_pending = true;
    } else {
        s_display_init_pending = false;
    }
}

// ---------------------------------------------------------------------------
// Flight loop
// ---------------------------------------------------------------------------

static float flightLoopCB(float /*elapsed*/, float /*flightLoop*/, int /*count*/, void*) {
    if (g_conn)    g_conn->poll();
    if (g_conn)    g_conn->tickBacklights();
    if (g_display) g_display->tick();

    // Auto-retry display init if G1000 wasn't bound at startup
    if (s_display_init_pending && (!g_display || !g_display->isReady())) {
        double t = Platform::now_seconds();
        if (t - s_last_retry_time >= 2.0) {  // retry every 2 seconds
            s_last_retry_time = t;
            initDisplay();
            if (g_display && g_display->isReady()) {
                s_display_init_pending = false;
                XPLMDebugString("[X1000] DisplayStreamer auto-init succeeded.\n");
            }
        }
    }

    // Update UI stats once per second
    double t = 0;
    {
        static double last = 0;
        t = Platform::now_seconds();
        if (t - last >= 1.0) {
            s_pfd_fps = s_pfd_frame_count;
            s_mfd_fps = s_mfd_frame_count;
            s_pfd_frame_count = s_mfd_frame_count = 0;
            last = t;
        }
    }

    if (g_ui) {
        g_ui->tick(g_display && g_display->isReady(),
                   g_display && g_display->isReady(),
                   s_pfd_fps, s_mfd_fps,
                   s_pfd_kb,  s_mfd_kb);

        // Detect when user closes the window via the X button
        // (XPLM has no close callback, so we poll)
        static bool s_last_visible = false;
        bool now_visible = g_ui->isVisible();
        if (s_last_visible && !now_visible) {
            // Window was just closed — save state
            g_settings->get().window_visible = false;
            g_settings->save();
        }
        s_last_visible = now_visible;
    }

    return -1.0f;
}

// ---------------------------------------------------------------------------
// XPluginStart
// ---------------------------------------------------------------------------

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    strncpy(outName, "X1000 Display Mirror",                                   255);
    strncpy(outSig,  "com.x1000.display",                                      255);
    strncpy(outDesc, "G1000 PFD/MFD mirror to iPads; bezel input; auto relay", 255);
    XPLMDebugString("[X1000] XPluginStart\n");

    // Get plugin directory
    char path[512]; XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
    std::string full(path);
    auto slash = full.rfind('/');
    g_plugin_dir = (slash != std::string::npos) ? full.substr(0, slash) : full;

    return 1;
}

PLUGIN_API void XPluginStop() {
    XPLMDebugString("[X1000] XPluginStop\n");
}

// ---------------------------------------------------------------------------
// XPluginEnable
// ---------------------------------------------------------------------------

PLUGIN_API int XPluginEnable() {
    XPLMDebugString("[X1000] XPluginEnable\n");

    // Settings
    g_settings = new SettingsManager();
    g_settings->init(g_plugin_dir);

    // Connection (bezel input)
    g_conn = new ConnectionManager();
    if (!g_conn->init()) {
        XPLMDebugString("[X1000] ConnectionManager init failed\n");
        delete g_conn; g_conn = nullptr;
    }

    // Relay — script lives at:
    //   <XP12>/Resources/plugins/X1000_display/tools/x1000_relay.py
    // g_plugin_dir is .../X1000_display/lin_x64 so go up one level
    g_relay = new RelayManager();
    std::string relay_path = g_plugin_dir + "/../tools/x1000_relay.py";
    g_relay->init(relay_path,
                  g_settings->get().relay_pfd_port,
                  g_settings->get().relay_mfd_port);
    g_relay->start();

    // Bezel BLE bridge
    g_bezel_relay = new RelayManager();
    {
        std::string bezel_path = g_plugin_dir + "/../tools/x1000_bezel.py";
        const Settings& bs = g_settings->get();
        std::string bezel_args;
        if (!bs.pfd_bezel_mac.empty())
            bezel_args += "--pfd " + bs.pfd_bezel_mac + " ";
        if (!bs.mfd_bezel_mac.empty())
            bezel_args += "--mfd " + bs.mfd_bezel_mac + " ";
        bezel_args += "--plugin-ip 127.0.0.1";

        g_bezel_relay->init(bezel_path,
                            bs.bezel_pfd_port,
                            bs.bezel_mfd_port,
                            bezel_args);

        if (!bs.pfd_bezel_mac.empty() || !bs.mfd_bezel_mac.empty()) {
            XPLMDebugString("[X1000] Bezel: disconnecting any existing BLE connections...\n");
            // Force BlueZ to disconnect from bezels so bleak can reconnect cleanly
            if (!bs.pfd_bezel_mac.empty()) {
                std::string cmd = "bluetoothctl disconnect " + bs.pfd_bezel_mac + " 2>/dev/null";
                system(cmd.c_str());
            }
            if (!bs.mfd_bezel_mac.empty()) {
                std::string cmd = "bluetoothctl disconnect " + bs.mfd_bezel_mac + " 2>/dev/null";
                system(cmd.c_str());
            }
            system("sleep 1");
            XPLMDebugString("[X1000] Bezel: starting BLE bridge...\n");
            g_bezel_relay->start();
        } else {
            XPLMDebugString("[X1000] Bezel: no MAC configured in ini\n");
        }
    }

    // UI — window visibility driven purely by saved setting, no extra save here
    g_ui = new UIManager();
    g_ui->init(g_settings, g_relay, g_bezel_relay, applySettings);
    // Only show if explicitly saved as visible (default is false)
    if (g_settings->get().window_visible) {
        g_ui->show();
    }

    // Display streamer
    initDisplay();

    XPLMRegisterFlightLoopCallback(flightLoopCB, -1.0f, nullptr);
    XPLMDebugString("[X1000] Plugin enabled.\n");
    return 1;
}

// ---------------------------------------------------------------------------
// XPluginDisable
// ---------------------------------------------------------------------------

PLUGIN_API void XPluginDisable() {
    XPLMDebugString("[X1000] XPluginDisable\n");
    XPLMUnregisterFlightLoopCallback(flightLoopCB, nullptr);

    if (g_ui)          { delete g_ui;             g_ui          = nullptr; }
    if (g_bezel_relay) { g_bezel_relay->stop(); delete g_bezel_relay; g_bezel_relay = nullptr; }
    if (g_relay)       { g_relay->stop();       delete g_relay;       g_relay       = nullptr; }
    if (g_display) { g_display->shutdown(); delete g_display; g_display = nullptr; }
    if (g_conn)    { g_conn->shutdown(); delete g_conn; g_conn = nullptr; }
    if (g_settings){ g_settings->save(); delete g_settings; g_settings = nullptr; }
}

// ---------------------------------------------------------------------------
// XPluginReceiveMessage
// ---------------------------------------------------------------------------

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int msg, void*) {
    if (msg == XPLM_MSG_PLANE_LOADED) {
        XPLMDebugString("[X1000] Aircraft loaded — queuing DisplayStreamer init\n");
        // Don't init immediately — avionics take a moment to bind after plane load.
        // Set pending flag so flight loop retries every 2 seconds.
        s_display_init_pending = true;
        s_last_retry_time = 0.0; // retry immediately on next flight loop tick

        // Auto-start relay if needed
        if (g_settings && g_settings->get().autostart &&
            g_relay && !g_relay->isRunning()) {
            g_relay->start();
        }
    }
}
