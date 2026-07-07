// RelayManager.cpp — Spawn, monitor, and log the Python relay process
// Uses Platform:: for cross-platform process management

#include "RelayManager.h"
#include "Platform.h"
#include <XPLMUtilities.h>
#if defined(LIN) || defined(__APPLE__)
#  include <unistd.h>   // access(), F_OK
#elif defined(_WIN32)
#  include <io.h>       // _access()
#endif
#include <cstdio>
#include <cstring>

RelayManager::RelayManager()
    : m_pfd_port(9000), m_mfd_port(9001)
    , m_monitor_running(false)
    , m_extra_args("")
{}

RelayManager::~RelayManager() { stop(); }

void RelayManager::init(const std::string& path,
                        uint16_t pfd_port, uint16_t mfd_port,
                        const std::string& extra_args) {
    m_script_path = path;
    m_pfd_port    = pfd_port;
    m_mfd_port    = mfd_port;
    m_extra_args  = extra_args;
}

bool RelayManager::start() {
    if (isRunning()) return true;

    // Kill any orphaned instance of the same script before starting
    // (handles case where X-Plane crashed and left the process running)
    std::string script_name = m_script_path;
    auto slash = script_name.rfind('/');
    if (slash != std::string::npos) script_name = script_name.substr(slash + 1);
    if (!script_name.empty()) {
        std::string kill_cmd = "pkill -f \"" + script_name + "\" 2>/dev/null";
        { int r = system(kill_cmd.c_str()); (void)r; }
        // Longer wait for bezel script so BLE stack releases connections
        if (script_name.find("bezel") != std::string::npos)
            { int r = system("sleep 2"); (void)r; }
        else
            { int r = system("sleep 0.5"); (void)r; }
        { int r = system(kill_cmd.c_str()); (void)r; }
    }

    // Check script exists
#if defined(_WIN32)
    if (_access(m_script_path.c_str(), 0) != 0) {
#else
    if (access(m_script_path.c_str(), F_OK) != 0) {
#endif
        std::string msg = "[X1000] Relay: script not found: " + m_script_path + "\n";
        XPLMDebugString(msg.c_str());
        addLog("Script not found: " + m_script_path, true);
        return false;
    }

    char pfd_str[8], mfd_str[8];
    snprintf(pfd_str, sizeof(pfd_str), "%u", m_pfd_port);
    snprintf(mfd_str, sizeof(mfd_str), "%u", m_mfd_port);

    std::vector<std::string> args = {
        Platform::pythonExecutable(),
        m_script_path,
        "--pfd-port", pfd_str,
        "--mfd-port", mfd_str
    };
    // Append extra args (e.g. --pfd MAC --mfd MAC for bezel script)
    if (!m_extra_args.empty()) {
        // Split on spaces
        std::string token;
        for (char c : m_extra_args) {
            if (c == ' ') {
                if (!token.empty()) { args.push_back(token); token.clear(); }
            } else {
                token += c;
            }
        }
        if (!token.empty()) args.push_back(token);
    }

    m_handle = Platform::spawnProcess(args);
    if (!m_handle.isValid()) {
        XPLMDebugString("[X1000] Relay: process launch failed\n");
        addLog("Process launch failed", true);
        return false;
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "[X1000] Relay started\n");
    XPLMDebugString(buf);
    addLog("Relay started", false);

    m_monitor_running = true;
    m_monitor = std::thread(&RelayManager::monitorThread, this);
    return true;
}

void RelayManager::stop() {
    // Set flag BEFORE killing so monitor thread knows this is intentional
    m_monitor_running = false;
    if (m_handle.isValid()) {
        Platform::killProcess(m_handle);
        addLog("Relay stopped.", false);
        XPLMDebugString("[X1000] Relay stopped.\n");
    }
    if (m_monitor.joinable()) m_monitor.join();
}

bool RelayManager::isRunning() const {
    return m_handle.isValid() && Platform::isProcessAlive(m_handle);
}

void RelayManager::restart_with_args(const std::string& args) {
    stop();
    m_extra_args = args;
    start();
}

void RelayManager::restart_scan() {
    // Launch bezel script in scan mode — outputs BEZEL_FOUND:MAC:NAME lines
    // Script exits after scan — this is expected, not an error
    stop();
    std::string old_extra = m_extra_args;
    m_extra_args = "--scan --plugin-ip 127.0.0.1";
    start();
    m_extra_args = old_extra;
}

void RelayManager::restart(uint16_t pfd_port, uint16_t mfd_port) {
    stop();
    m_pfd_port = pfd_port;
    m_mfd_port = mfd_port;
    start();
}

void RelayManager::monitorThread() {
    std::string partial;

    while (m_monitor_running) {
        std::string chunk = Platform::readProcessOutput(m_handle, 200);

        if (chunk.empty()) {
            if (m_handle.isValid() && !Platform::isProcessAlive(m_handle)) {
                if (m_monitor_running) {
                    // Only flag as unexpected if not a scan (scan exits normally)
                    bool is_scan = (m_extra_args.find("--scan") != std::string::npos);
                    if (!is_scan) {
                        addLog("Relay process exited unexpectedly.", true);
                        XPLMDebugString("[X1000] Relay process exited unexpectedly.\n");
                    }
                }
                break;
            }
            continue;
        }

        partial += chunk;
        size_t pos;
        while ((pos = partial.find('\n')) != std::string::npos) {
            std::string line = partial.substr(0, pos);
            partial = partial.substr(pos + 1);
            if (line.empty()) continue;

            bool is_err = line.find("ERROR")     != std::string::npos
                       || line.find("Error")     != std::string::npos
                       || line.find("Traceback") != std::string::npos
                       || line.find("Exception") != std::string::npos;

            addLog(line, is_err);

            if (is_err
             || line.find("first frame") != std::string::npos
             || line.find("listening")   != std::string::npos) {
                std::string xplog = "[X1000] Relay: " + line + "\n";
                XPLMDebugString(xplog.c_str());
            }
        }
    }
}

void RelayManager::addLog(const std::string& text, bool is_error) {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    if (m_log.size() >= MAX_LOG_LINES)
        m_log.erase(m_log.begin());
    m_log.push_back({text, is_error});
}

std::vector<RelayLogEntry> RelayManager::getLog() {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    return m_log;
}

void RelayManager::clearLog() {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    m_log.clear();
}
