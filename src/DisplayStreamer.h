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
// On 64-bit Windows, SOCKET is UINT_PTR (unsigned 64-bit).
// Using 'int' would truncate the handle and corrupt subsequent calls.
typedef SOCKET sock_t;
static const sock_t INVALID_SOCK = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
typedef int sock_t;
static const sock_t INVALID_SOCK = -1;
#endif
#include <XPLMDisplay.h>
#include "Platform.h"   // GL types (GLuint etc.)

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
    static bool hasG1000Avionics();  // true if G1000 handles exist in this aircraft

    // Called by static avionics draw callbacks (viewport snapshot only, no GL readback)
    void onDrawPFD();
    void onDrawMFD();
    // Called by scene-phase draw callback (safe point for glReadPixels under Zink)
    void onScenePhase();

private:
    bool acquireHandles();
    bool registerDrawCallbacks();

    void captureAndSend(int rx, int ry, int rw, int rh,
                        GLuint fbo,
                        sock_t sock, sockaddr_in& addr,
                        const char* name);
    void pushFrame(const uint8_t* data, size_t len,
                   sock_t sock, sockaddr_in& addr);

    // Unused ffmpeg stubs
    bool startFfmpeg();
    void stopFfmpeg();
    bool popOutAndGetGeometry(XPLMAvionicsID handle, const char* name,
                              int& x, int& y, int& w, int& h);

    XPLMAvionicsID m_pfd_handle     = nullptr;
    XPLMAvionicsID m_mfd_handle     = nullptr;
    XPLMAvionicsID m_pfd_cb_handle  = nullptr; // handle from RegisterCallbacksEx
    XPLMAvionicsID m_mfd_cb_handle  = nullptr;

    sock_t      m_pfd_sock = INVALID_SOCK;
    sock_t      m_mfd_sock = INVALID_SOCK;
    sockaddr_in m_pfd_addr = {};
    sockaddr_in m_mfd_addr = {};

    uint32_t     m_frame_seq    = 0;
    double       m_last_pfd_time = 0.0;
    double       m_last_mfd_time = 0.0;

    StreamConfig m_cfg;
    bool         m_ready = false;
    bool         m_scene_cb_registered = false;
    double       m_last_frame_time = 0.0;
};
