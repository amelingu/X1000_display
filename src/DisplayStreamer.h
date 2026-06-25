#pragma once
// DisplayStreamer.h — Option A: Avionics draw callbacks + JPEG + UDP push
//
// Uses XPLMRegisterAvionicsCallbacksEx() drawCallbackAfter to capture
// G1000 frames directly from the GL framebuffer at render time.
// XPLMGetAvionicsScreenSize() gives panel dimensions without geometry queries.
// Frames are JPEG-encoded with stb_image_write and pushed as UDP to iPads.

#include <string>
#include <cstdint>
#include <vector>
#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#endif
#include <XPLMDisplay.h>

enum class StreamMethod { XPLM, FFMPEG };

struct StreamConfig {
    StreamMethod method        = StreamMethod::XPLM;
    std::string  pfd_ip        = "127.0.0.1"; // relay on same PC
    std::string  mfd_ip        = "127.0.0.1";
    uint16_t     pfd_push_port = 9000;
    uint16_t     mfd_push_port = 9001;

    // Quality tuning — adjust these to balance FPS vs image quality
    int    jpeg_quality  = 85;   // 0-100 (85 = good quality, ~60KB/frame)
    int    stream_width  = 1024; // output width in pixels (height auto from aspect)
    double fps           = 15.0; // frames per second
};

class DisplayStreamer {
public:
    DisplayStreamer();
    ~DisplayStreamer();

    bool init(const StreamConfig& cfg);
    void tick();
    void shutdown();
    bool isReady() const { return m_ready; }

    // Called by static draw callbacks registered with XPLMRegisterAvionicsCallbacksEx
    void onDrawPFD();
    void onDrawMFD();

private:
    bool acquireHandles();
    bool registerDrawCallbacks();

    void captureAndSend(int w, int h, int sock, sockaddr_in& addr,
                        const char* name);
    void pushFrame(const uint8_t* data, size_t len,
                   int sock, sockaddr_in& addr);

    // Unused ffmpeg stubs
    bool startFfmpeg();
    void stopFfmpeg();
    bool popOutAndGetGeometry(XPLMAvionicsID handle, const char* name,
                              int& x, int& y, int& w, int& h);

    XPLMAvionicsID m_pfd_handle     = nullptr;
    XPLMAvionicsID m_mfd_handle     = nullptr;
    XPLMAvionicsID m_pfd_cb_handle  = nullptr; // handle from RegisterCallbacksEx
    XPLMAvionicsID m_mfd_cb_handle  = nullptr;

    int         m_pfd_sock = -1;
    int         m_mfd_sock = -1;
    sockaddr_in m_pfd_addr = {};
    sockaddr_in m_mfd_addr = {};

    uint32_t     m_frame_seq    = 0;
    double       m_last_pfd_time = 0.0;
    double       m_last_mfd_time = 0.0;

    StreamConfig m_cfg;
    bool         m_ready = false;
    double       m_last_frame_time = 0.0;


    // fps and stream_width are in StreamConfig
};
