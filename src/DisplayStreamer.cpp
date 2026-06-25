// DisplayStreamer.cpp — Option A: Avionics draw callback + JPEG + UDP push
//
// Correct approach for capturing built-in G1000 screens:
//   XPLMRegisterAvionicsCallbacksEx() with drawCallbackAfter
//   → X-Plane calls our callback after rendering the G1000
//   → Inside the callback, glReadPixels at (0,0,w,h) captures the panel
//   → We encode to JPEG and push via UDP to the iPad
//
// Screen dimensions from XPLMGetAvionicsScreenSize() (XPLM410).
// No geometry queries needed — in the draw callback, panel coords are always
// (0,0) to (screenWidth, screenHeight).

#include "DisplayStreamer.h"
#include <XPLMUtilities.h>
#include <XPLMGraphics.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <string>
#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define close(s) closesocket(s)   // for socket close only
   static void usleep(unsigned us) { Sleep(us/1000 > 0 ? us/1000 : 1); }
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/socket.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <errno.h>
#endif
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include "Platform.h"   // GL includes + loadGLBufferFunctions + cross-platform utils

// Platform.h defines glGenBuffers etc. as macros pointing to Platform::glGenBuffers_

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma GCC diagnostic pop

static const uint32_t MAGIC_FRAME = 0x464B3158; // "X1KF" LE
static const uint32_t MAGIC_CHUNK = 0x434B3158; // "X1KC" LE
static const size_t   UDP_MTU     = 1400;
static const size_t   CHUNK_HDR   = 20;
static const size_t   CHUNK_DATA  = UDP_MTU - CHUNK_HDR;

// ---------------------------------------------------------------------------
// Background encode/send worker
// The draw callback runs on X-Plane's render thread.
// We hand off scaled pixel data to a worker thread for JPEG encode + UDP send.
// This keeps the render thread cost to: PBO readback + pixel scaling only.
// ---------------------------------------------------------------------------

struct EncodeJob {
    std::vector<uint8_t> pixels;  // scaled, flipped, RGB
    int    w, h;
    int    quality;
    int    sock;
    sockaddr_in addr;
};

static std::queue<EncodeJob>    s_encode_queue;
static std::mutex               s_queue_mutex;
static std::condition_variable  s_queue_cv;
static std::atomic<bool>        s_worker_running{false};
static std::thread              s_worker_thread;

static void jpegWriteCB_worker(void* ctx, void* data, int size) {
    auto* buf = (std::vector<uint8_t>*)ctx;
    auto* ptr = (uint8_t*)data;
    buf->insert(buf->end(), ptr, ptr + size);
}

static void workerFunc() {
    while (s_worker_running) {
        EncodeJob job;
        {
            std::unique_lock<std::mutex> lock(s_queue_mutex);
            s_queue_cv.wait_for(lock, std::chrono::milliseconds(100),
                                [] { return !s_encode_queue.empty() || !s_worker_running; });
            if (s_encode_queue.empty()) continue;
            job = std::move(s_encode_queue.front());
            s_encode_queue.pop();
        }

        // Encode JPEG
        std::vector<uint8_t> jpeg;
        jpeg.reserve(job.w * job.h / 4);
        stbi_write_jpg_to_func(jpegWriteCB_worker, &jpeg,
                               job.w, job.h, 3,
                               job.pixels.data(), job.quality);
        if (jpeg.empty()) continue;

        static bool worker_logged = false;
        if (!worker_logged) {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "[X1000] worker: first JPEG encoded %zu bytes\n", jpeg.size());
            XPLMDebugString(buf);
            worker_logged = true;
        }

        // UDP push with chunking
        // Use global CHUNK_DATA / CHUNK_HDR constants
        uint32_t seq = 0; // worker-local seq is fine — each display has its own socket
        {
            // Use a single monotonic seq per job - addr distinguishes displays
            static std::atomic<uint32_t> global_seq{0};
            seq = global_seq++;
        }

        size_t len   = jpeg.size();
        uint32_t total = (uint32_t)((len + CHUNK_DATA - 1) / CHUNK_DATA);

        if (total == 1) {
            std::vector<uint8_t> pkt(12 + len);
            uint32_t magic = 0x464B3158, dlen = (uint32_t)len;
            memcpy(pkt.data()+0, &magic, 4);
            memcpy(pkt.data()+4, &seq,   4);
            memcpy(pkt.data()+8, &dlen,  4);
            memcpy(pkt.data()+12, jpeg.data(), len);
            ssize_t sent = sendto(job.sock, (const char*)pkt.data(), (int)pkt.size(), 0,
                   (sockaddr*)&job.addr, sizeof(job.addr));
            if (sent < 0) {
                static bool err_logged = false;
                if (!err_logged) {
                    char ebuf[64];
                    snprintf(ebuf, sizeof(ebuf),
                             "[X1000] worker sendto failed: errno=%d\n", errno);
                    XPLMDebugString(ebuf);
                    err_logged = true;
                }
            }
        } else {
            for (uint32_t i = 0; i < total; ++i) {
                size_t off  = i * CHUNK_DATA;
                size_t clen = std::min(CHUNK_DATA, len - off);
                std::vector<uint8_t> pkt(CHUNK_HDR + clen);
                uint32_t magic = 0x434B3158, dlen32 = (uint32_t)clen;
                memcpy(pkt.data()+ 0, &magic,  4);
                memcpy(pkt.data()+ 4, &seq,    4);
                memcpy(pkt.data()+ 8, &i,      4);
                memcpy(pkt.data()+12, &total,  4);
                memcpy(pkt.data()+16, &dlen32, 4);
                memcpy(pkt.data()+20, jpeg.data()+off, clen);
                sendto(job.sock, (const char*)pkt.data(), (int)pkt.size(), 0,
                       (sockaddr*)&job.addr, sizeof(job.addr));
            }
        }
    }
}

static void startWorker() {
    if (s_worker_running) return;
    s_worker_running = true;
    s_worker_thread = std::thread(workerFunc);
    XPLMDebugString("[X1000] Encode worker thread started.\n");
}

static void stopWorker() {
    if (!s_worker_running) return;
    s_worker_running = false;
    s_queue_cv.notify_all();
    if (s_worker_thread.joinable()) s_worker_thread.join();
    XPLMDebugString("[X1000] Encode worker thread stopped.\n");
}

// Platform::now_seconds() provided by Platform::

static void jpegWriteCB(void* ctx, void* data, int size) {
    auto* buf = (std::vector<uint8_t>*)ctx;
    auto* ptr = (uint8_t*)data;
    buf->insert(buf->end(), ptr, ptr + size);
}

// ---------------------------------------------------------------------------
// Static draw callbacks — called by X-Plane after rendering each G1000
// ---------------------------------------------------------------------------

// Forward declaration
static DisplayStreamer* g_streamer = nullptr;

static int drawCallbackPFD(XPLMDeviceID /*device*/,
                           int          /*isBefore*/,
                           void*        /*refcon*/) {
    if (g_streamer) g_streamer->onDrawPFD();
    return 1; // 1 = let X-Plane draw normally
}

static int drawCallbackMFD(XPLMDeviceID /*device*/,
                           int          /*isBefore*/,
                           void*        /*refcon*/) {
    if (g_streamer) g_streamer->onDrawMFD();
    return 1;
}

// ---------------------------------------------------------------------------

DisplayStreamer::DisplayStreamer()
    : m_pfd_handle(nullptr)
    , m_mfd_handle(nullptr)
    , m_ready(false)
    , m_last_frame_time(0.0)
    , m_frame_seq(0)
    , m_pfd_sock(-1)
    , m_mfd_sock(-1)
{
    g_streamer = this;
}

DisplayStreamer::~DisplayStreamer() {
    g_streamer = nullptr;
    shutdown();
}

// ---------------------------------------------------------------------------
// UDP socket setup
// ---------------------------------------------------------------------------

static int makeUDPSocket(const std::string& ip, uint16_t port,
                         sockaddr_in& out_addr) {
    int fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    memset(&out_addr, 0, sizeof(out_addr));
    out_addr.sin_family = AF_INET;
    out_addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip.c_str(), &out_addr.sin_addr);
    return fd;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool DisplayStreamer::acquireHandles() {
    // Use XPLMGetAvionicsHandle — no callbacks, just interaction
    // We'll register draw callbacks separately via XPLMRegisterAvionicsCallbacksEx
    m_pfd_handle = XPLMGetAvionicsHandle(xplm_device_G1000_PFD_1);
    m_mfd_handle = XPLMGetAvionicsHandle(xplm_device_G1000_MFD);

    if (!m_pfd_handle || !m_mfd_handle) {
        XPLMDebugString("[X1000] XPLMGetAvionicsHandle returned NULL\n");
        return false;
    }

    int pfd_bound = 0, mfd_bound = 0;
    for (int i = 0; i < 10; ++i) {
        pfd_bound = XPLMIsAvionicsBound(m_pfd_handle);
        mfd_bound = XPLMIsAvionicsBound(m_mfd_handle);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "[X1000] Avionics bound (attempt %d): PFD=%d MFD=%d\n",
                 i+1, pfd_bound, mfd_bound);
        XPLMDebugString(buf);
        if (pfd_bound && mfd_bound) break;
        usleep(200000);
    }
    if (!pfd_bound || !mfd_bound) {
        XPLMDebugString("[X1000] G1000 not bound — load a G1000 aircraft.\n");
        return false;
    }
    return true;
}

bool DisplayStreamer::registerDrawCallbacks() {
    // Register drawCallbackAfter for PFD
    XPLMCustomizeAvionics_t pfd_params = {};
    pfd_params.structSize         = sizeof(XPLMCustomizeAvionics_t);
    pfd_params.deviceId           = xplm_device_G1000_PFD_1;
    pfd_params.drawCallbackBefore = nullptr;
    pfd_params.drawCallbackAfter  = drawCallbackPFD;
    pfd_params.refcon             = nullptr;

    m_pfd_cb_handle = XPLMRegisterAvionicsCallbacksEx(&pfd_params);
    if (!m_pfd_cb_handle) {
        XPLMDebugString("[X1000] Failed to register PFD draw callback\n");
        return false;
    }

    // Register drawCallbackAfter for MFD
    XPLMCustomizeAvionics_t mfd_params = {};
    mfd_params.structSize         = sizeof(XPLMCustomizeAvionics_t);
    mfd_params.deviceId           = xplm_device_G1000_MFD;
    mfd_params.drawCallbackBefore = nullptr;
    mfd_params.drawCallbackAfter  = drawCallbackMFD;
    mfd_params.refcon             = nullptr;

    m_mfd_cb_handle = XPLMRegisterAvionicsCallbacksEx(&mfd_params);
    if (!m_mfd_cb_handle) {
        XPLMDebugString("[X1000] Failed to register MFD draw callback\n");
        return false;
    }

    XPLMDebugString("[X1000] Draw callbacks registered for PFD and MFD.\n");
    return true;
}

bool DisplayStreamer::init(const StreamConfig& cfg) {
    m_cfg = cfg;

    if (!acquireHandles())        return false;
    if (!registerDrawCallbacks()) return false;

    m_pfd_sock = makeUDPSocket(cfg.pfd_ip, cfg.pfd_push_port, m_pfd_addr);
    m_mfd_sock = makeUDPSocket(cfg.mfd_ip, cfg.mfd_push_port, m_mfd_addr);

    if (m_pfd_sock < 0 || m_mfd_sock < 0) {
        XPLMDebugString("[X1000] Failed to create UDP sockets\n");
        return false;
    }

    startWorker();

    char buf[128];
    snprintf(buf, sizeof(buf),
             "[X1000] Option A ready: PFD->%s:%d MFD->%s:%d\n",
             cfg.pfd_ip.c_str(), cfg.pfd_push_port,
             cfg.mfd_ip.c_str(), cfg.mfd_push_port);
    XPLMDebugString(buf);

    m_ready = true;
    return true;
}

// ---------------------------------------------------------------------------
// tick — minimal, draw callbacks do the real work
// ---------------------------------------------------------------------------

void DisplayStreamer::tick() {
    // Capture is triggered by X-Plane's draw callbacks, not the flight loop.
    // tick() is kept for future use (e.g. watchdog, stats).
}

// ---------------------------------------------------------------------------
// Draw callbacks — called by X-Plane after rendering each G1000 panel
// In this context, the GL framebuffer contains the panel at (0,0,w,h)
// ---------------------------------------------------------------------------

// Helper: get screen dimensions from GeometryOS (works when popped out)
static bool getScreenSize(XPLMAvionicsID handle, int& w, int& h) {
    int left = 0, top = 0, right = 0, bottom = 0;
    XPLMGetAvionicsGeometryOS(handle, &left, &top, &right, &bottom);
    w = right - left;
    h = bottom - top; // OS coords: bottom > top
    if (w <= 0) w = -w; // normalise if inverted
    if (h <= 0) h = -h;
    w &= ~1; h &= ~1;   // even for JPEG
    return (w > 0 && h > 0);
}

void DisplayStreamer::onDrawPFD() {
    if (!m_ready || m_pfd_sock < 0) return;

    double t = Platform::now_seconds();
    if ((t - m_last_pfd_time) < (1.0 / m_cfg.fps)) return;
    m_last_pfd_time = t;

    int w = 0, h = 0;
    if (!getScreenSize(m_pfd_handle, w, h)) return;
    captureAndSend(w, h, m_pfd_sock, m_pfd_addr, "PFD");
}

void DisplayStreamer::onDrawMFD() {
    if (!m_ready || m_mfd_sock < 0) return;

    double t = Platform::now_seconds();
    if ((t - m_last_mfd_time) < (1.0 / m_cfg.fps)) return;
    m_last_mfd_time = t;

    int w = 0, h = 0;
    if (!getScreenSize(m_mfd_handle, w, h)) return;
    captureAndSend(w, h, m_mfd_sock, m_mfd_addr, "MFD");
}

// ---------------------------------------------------------------------------
// PBO async readback
// We use a double-buffer PBO scheme:
//   Frame N:   glReadPixels into PBO[0] (async, non-blocking)
//   Frame N+1: map PBO[1] (ready from previous frame), encode, send
// This completely hides the GPU→CPU transfer latency.
// ---------------------------------------------------------------------------

struct PBOState {
    GLuint pbo[2]  = {0, 0};
    int    index   = 0;      // which PBO we write to this frame
    int    rw      = 0;
    int    rh      = 0;
    bool   ready   = false;  // true once both PBOs have been filled once
    int    fill_count = 0;
};

void DisplayStreamer::captureAndSend(int w, int h,
                                     int sock, sockaddr_in& addr,
                                     const char* name) {
    // Load GL buffer functions on first call (PBO requires GL 1.5)
    if (!Platform::loadGLBufferFunctions()) {
        // Fallback: synchronous glReadPixels without PBO
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        int rx=vp[0],ry=vp[1],rw=(vp[2]>0)?vp[2]:w,rh=(vp[3]>0)?vp[3]:h;
        rw&=~1; rh&=~1;
        const int TARGET_W=m_cfg.stream_width&~1;
        const int TARGET_H=(rh*TARGET_W/rw)&~1;
        std::vector<uint8_t> pixels(rw*rh*3);
        glReadPixels(rx,ry,rw,rh,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
        std::vector<uint8_t> scaled(TARGET_W*TARGET_H*3);
        float sx=(float)rw/TARGET_W, sy=(float)rh/TARGET_H;
        for(int dy=0;dy<TARGET_H;++dy){
            int ssy=(int)((TARGET_H-1-dy)*sy); if(ssy>=rh)ssy=rh-1;
            for(int dx=0;dx<TARGET_W;++dx){
                int ssx=(int)(dx*sx); if(ssx>=rw)ssx=rw-1;
                memcpy(&scaled[(dy*TARGET_W+dx)*3],&pixels[(ssy*rw+ssx)*3],3);
            }
        }
        std::vector<uint8_t> jpeg; jpeg.reserve(TARGET_W*TARGET_H/4);
        stbi_write_jpg_to_func(jpegWriteCB,&jpeg,TARGET_W,TARGET_H,3,scaled.data(),m_cfg.jpeg_quality);
        if(!jpeg.empty()) pushFrame(jpeg.data(),jpeg.size(),sock,addr);
        return;
    }

    // Get viewport
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);

    int rx = vp[0], ry = vp[1];
    int rw = (vp[2] > 0) ? vp[2] : w;
    int rh = (vp[3] > 0) ? vp[3] : h;
    rw &= ~1; rh &= ~1;

    // Per-display PBO state (static, one per name initial letter)
    static PBOState pfd_pbo, mfd_pbo;
    PBOState& pbo = (name[0] == 'P') ? pfd_pbo : mfd_pbo;

    // (Re)allocate PBOs if size changed
    if (pbo.pbo[0] == 0 || pbo.rw != rw || pbo.rh != rh) {
        if (pbo.pbo[0]) glDeleteBuffers(2, pbo.pbo);
        glGenBuffers(2, pbo.pbo);
        size_t sz = rw * rh * 3;
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo.pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, sz, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        pbo.rw = rw; pbo.rh = rh;
        pbo.ready = false; pbo.fill_count = 0;
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "[X1000] %s PBO allocated: %dx%d\n", name, rw, rh);
        XPLMDebugString(buf);
    }

    int write_idx = pbo.index;
    int read_idx  = 1 - pbo.index;

    // --- Step 1: kick off async readback into write PBO (returns immediately) ---
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo.pbo[write_idx]);
    glReadPixels(rx, ry, rw, rh, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    pbo.fill_count++;
    pbo.index = read_idx; // swap for next frame

    // --- Step 2: map read PBO from previous frame (data is ready, no stall) ---
    if (pbo.fill_count < 2) return; // need at least 2 frames to have valid read PBO

    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo.pbo[read_idx]);
    const uint8_t* pixels = (const uint8_t*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (!pixels) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return;
    }

    // Scale and flip
    const int TARGET_W = m_cfg.stream_width & ~1;
    const int TARGET_H = (rh * TARGET_W / rw) & ~1;
    std::vector<uint8_t> scaled(TARGET_W * TARGET_H * 3);
    float scaleX = (float)rw / TARGET_W;
    float scaleY = (float)rh / TARGET_H;
    for (int dy = 0; dy < TARGET_H; ++dy) {
        int sy = (int)((TARGET_H - 1 - dy) * scaleY);
        if (sy >= rh) sy = rh - 1;
        for (int dx = 0; dx < TARGET_W; ++dx) {
            int sx = (int)(dx * scaleX);
            if (sx >= rw) sx = rw - 1;
            const uint8_t* src = &pixels[(sy * rw + sx) * 3];
            uint8_t* dst = &scaled[(dy * TARGET_W + dx) * 3];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        }
    }

    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    // Log first frame dimensions
    static bool pfd_logged = false, mfd_logged = false;
    bool& logged = (name[0] == 'P') ? pfd_logged : mfd_logged;
    if (!logged) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[X1000] %s first frame: %dx%d -> %dx%d\n",
                 name, rw, rh, TARGET_W, TARGET_H);
        XPLMDebugString(buf);
        logged = true;
    }

    // Hand off to background worker — render thread is now free
    // Drop frame if queue is backing up (keep latency low)
    {
        std::unique_lock<std::mutex> lock(s_queue_mutex);
        if (s_encode_queue.size() < 2) {  // max 2 frames queued
            EncodeJob job;
            job.pixels  = std::move(scaled);
            job.w       = TARGET_W;
            job.h       = TARGET_H;
            job.quality = m_cfg.jpeg_quality;
            job.sock    = sock;
            job.addr    = addr;
            s_encode_queue.push(std::move(job));
            s_queue_cv.notify_one();
        }
        // else: drop this frame — sim is more important than streaming
    }
}

// ---------------------------------------------------------------------------
// UDP push with chunking
// ---------------------------------------------------------------------------

void DisplayStreamer::pushFrame(const uint8_t* data, size_t len,
                                int sock, sockaddr_in& addr) {
    uint32_t seq   = m_frame_seq++;
    uint32_t total = (uint32_t)((len + CHUNK_DATA - 1) / CHUNK_DATA);

    if (total == 1) {
        std::vector<uint8_t> pkt(12 + len);
        uint32_t magic = MAGIC_FRAME, dlen = (uint32_t)len;
        memcpy(pkt.data() + 0, &magic, 4);
        memcpy(pkt.data() + 4, &seq,   4);
        memcpy(pkt.data() + 8, &dlen,  4);
        memcpy(pkt.data() + 12, data, len);
        sendto(sock, (const char*)pkt.data(), (int)pkt.size(), 0,
               (sockaddr*)&addr, sizeof(addr));
        return;
    }

    for (uint32_t i = 0; i < total; ++i) {
        size_t offset    = i * CHUNK_DATA;
        size_t chunk_len = std::min(CHUNK_DATA, len - offset);
        std::vector<uint8_t> pkt(CHUNK_HDR + chunk_len);
        uint32_t magic = MAGIC_CHUNK, dlen = (uint32_t)chunk_len;
        memcpy(pkt.data() +  0, &magic,          4);
        memcpy(pkt.data() +  4, &seq,             4);
        memcpy(pkt.data() +  8, &i,               4);
        memcpy(pkt.data() + 12, &total,           4);
        memcpy(pkt.data() + 16, &dlen,            4);
        memcpy(pkt.data() + 20, data + offset, chunk_len);
        sendto(sock, (const char*)pkt.data(), (int)pkt.size(), 0,
               (sockaddr*)&addr, sizeof(addr));
    }
}

// ---------------------------------------------------------------------------
// Stubs for unused ffmpeg path
// ---------------------------------------------------------------------------

bool DisplayStreamer::startFfmpeg()  { return false; }
void DisplayStreamer::stopFfmpeg()   {}
bool DisplayStreamer::popOutAndGetGeometry(XPLMAvionicsID, const char*,
                                           int&, int&, int&, int&) {
    return false;
}

// ---------------------------------------------------------------------------

void DisplayStreamer::shutdown() {
    if (m_pfd_cb_handle) {
        XPLMUnregisterAvionicsCallbacks(m_pfd_cb_handle);
        m_pfd_cb_handle = nullptr;
    }
    if (m_mfd_cb_handle) {
        XPLMUnregisterAvionicsCallbacks(m_mfd_cb_handle);
        m_mfd_cb_handle = nullptr;
    }
    stopWorker();
    // PBO cleanup handled implicitly by GL context cleanup on plugin unload
#if defined(_WIN32)
    if (m_pfd_sock >= 0) { closesocket(m_pfd_sock); m_pfd_sock = -1; }
    if (m_mfd_sock >= 0) { closesocket(m_mfd_sock); m_mfd_sock = -1; }
#else
    if (m_pfd_sock >= 0) { ::close(m_pfd_sock); m_pfd_sock = -1; }
    if (m_mfd_sock >= 0) { ::close(m_mfd_sock); m_mfd_sock = -1; }
#endif
    m_pfd_handle = nullptr;
    m_mfd_handle = nullptr;
    m_ready = false;
    XPLMDebugString("[X1000] DisplayStreamer: shutdown.\n");
}
