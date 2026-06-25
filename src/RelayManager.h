#pragma once
// RelayManager.h — Spawns and monitors the Python relay process.
// Uses Platform:: for cross-platform process management.

#include "Platform.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

struct RelayLogEntry {
    std::string text;
    bool        is_error;
};

class RelayManager {
public:
    RelayManager();
    ~RelayManager();

    void init(const std::string& relay_script_path,
              uint16_t pfd_port, uint16_t mfd_port);

    bool start();
    void stop();
    bool isRunning() const;

    void restart(uint16_t pfd_port, uint16_t mfd_port);

    std::vector<RelayLogEntry> getLog();
    void clearLog();

private:
    void monitorThread();
    void addLog(const std::string& text, bool is_error);

    std::string              m_script_path;
    uint16_t                 m_pfd_port;
    uint16_t                 m_mfd_port;
    Platform::ProcessHandle  m_handle;

    std::thread              m_monitor;
    std::atomic<bool>        m_monitor_running;
    std::vector<RelayLogEntry> m_log;
    std::mutex               m_log_mutex;

    static constexpr size_t MAX_LOG_LINES = 100;
};
