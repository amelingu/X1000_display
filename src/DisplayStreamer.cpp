// DisplayStreamer.cpp — G1000 capture: blit FBO + deferred readback + JPEG + UDP
//
// Fix 8 approach (Zink-safe):
//   1. XPLMRegisterAvionicsCallbacksEx() drawCallbackAfter.
//      Inside: glGetIntegerv(GL_VIEWPORT) once to learn panel size (CPU-side,
//      no GPU sync, no framebuffer state touched).  Then if our blit FBO is
//      ready: glBindFramebuffer(DRAW, our_fbo) + glBlitFramebuffer + restore
//      DRAW to 0 hardcoded (no glGetIntegerv for framebuffer binding).
//   2. XPLMRegisterDrawCallback(xplm_Phase_Window) — safe for GL state ops.
//      Allocates blit FBO on first use (lazy, from dimensions cached in step 1).
//      Then glReadPixels(0,0,w,h) from our own FBO — not X-Plane's.
//   3. JPEG encode + UDP chunked send run on a background worker thread.
//
// Key insight: XPLMGetAvionicsGeometry() returns popup window bounds, NOT the
// in-sim panel position. The only way to read G1000 pixels is from INSIDE the
// avionics callback where the GL context is already on the panel FBO.

#include "DisplayStreamer.h"
#include <XPLMUtilities.h>
#include <XPLMDataAccess.h>
#include <XPLMGraphics.h>
#include <XPLMDisplay.h>    // XPLMRegisterDrawCallback / XPLMUnregisterDrawCallback
#include <cstddef>
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

// Bring Platform function pointers into scope so glBindFramebuffer_ etc. resolve
// without needing the Platform:: prefix everywhere.
using Platform::glGenFramebuffers_;
using Platform::glDeleteFramebuffers_;
using Platform::glBindFramebuffer_;
using Platform::glFramebufferTexture2D_;
using Platform::glCheckFramebufferStatus_;
using Platform::glBlitFramebuffer_;

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
    int     w, h;
    int     quality;
    sock_t  sock;   // SOCKET on Windows (UINT_PTR/64-bit); int on POSIX
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
#if defined(_WIN32)
                    snprintf(ebuf, sizeof(ebuf),
                             "[X1000] worker sendto failed: WSAError=%d\n",
                             WSAGetLastError());
#else
                    snprintf(ebuf, sizeof(ebuf),
                             "[X1000] worker sendto failed: errno=%d\n", errno);
#endif
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

// ---------------------------------------------------------------------------
// Deferred FBO blit capture
//
// X-Plane 12.4.4 on Windows uses Mesa Zink (OpenGL over Vulkan).
// Both glReadPixels AND glMapBuffer crash Zink inside an avionics draw
// callback because they force pipeline flushes that Vulkan cannot handle
// safely while inside a render pass.
//
// Safe strategy: inside the avionics callback, blit the avionics FBO into
// an application-owned FBO (GPU→GPU, no readback, safe inside render pass),
// then call glReadPixels from a scene-phase callback AFTER all avionics
// have finished rendering, at which point we read from our own FBO —
// not X-Plane's — so the render pass is over and Zink is happy.
//
// Our blit FBO owns a single RGB texture sized to the avionics panel.
// It is created lazily on first use and recreated if the panel size changes.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fix 7: Zero-GL avionics callback approach
//
// Root cause: under Zink (Mesa OpenGL→Vulkan bridge), ANY GL call inside the
// avionics draw callback that touches framebuffer state — including
// glBlitFramebuffer and glBindFramebuffer — corrupts the Vulkan render pass.
// The corruption manifests as a crash on the SECOND frame.
//
// Solution: avionics callback does zero GL work — only sets a pending flag.
// The xplm_Phase_Window callback then calls XPLMGetAvionicsGeometry() for GL
// screen-space panel bounds, and reads directly from FBO 0 with glReadPixels.
// No blit FBO, no state queries — completely safe under Zink/Vulkan.
// ---------------------------------------------------------------------------

// DeferredCapture — carries panel state between avionics callback and scene phase.
// All fields written+read on the render thread only; no locking needed.
struct DeferredCapture {
    std::atomic<bool> pending{false};  // avionics callback sets true each frame
    GLint rw = 0, rh = 0;             // panel pixel dimensions (from GL_VIEWPORT)
    GLuint fbo = 0;                   // our blit-destination FBO
    GLuint tex = 0;                   // RGB8 texture attached to fbo
    std::atomic<bool> fbo_ready{false}; // fbo/tex allocated and sized correctly
};

static DeferredCapture s_pfd_capture;
static DeferredCapture s_mfd_capture;

// ---------------------------------------------------------------------------
// Static draw callbacks — called by X-Plane after rendering each G1000
// ---------------------------------------------------------------------------

// Forward declaration
static DisplayStreamer* g_streamer = nullptr;

// Avionics callbacks: ZERO GL calls under Zink.
// We only set a pending flag so onScenePhase() knows to capture this device.
// All GL work (geometry query + glReadPixels) happens in the safe Window phase.

// blitPanelToFBO — called from avionics draw callback (after X-Plane rendered).
// Zink-safe rules:
//   - glGetIntegerv(GL_VIEWPORT) is CPU-side state, no GPU sync, safe.
//   - glBindFramebuffer(DRAW, ...) + glBlitFramebuffer: GPU-only, no readback.
//   - Restore DRAW to 0 hardcoded — no glGetIntegerv(framebuffer) query.
//   - X-Plane re-binds its own framebuffers after our callback returns anyway.
static void blitPanelToFBO(DeferredCapture& cap) {
    // Step 1: learn panel size from viewport (once, or if it changed).
    GLint vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    GLint w = vp[2] & ~1;
    GLint h = vp[3] & ~1;
    if (w > 0 && h > 0) { cap.rw = w; cap.rh = h; }

    // Step 2: if our FBO is ready and sized correctly, do the blit.
    if (cap.fbo_ready.load(std::memory_order_acquire) &&
        cap.rw == w && cap.rh == h && w > 0 && h > 0) {
        // READ is the avionics panel FBO (set by X-Plane before our callback).
        // DRAW is ours. Blit panel → our texture.
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, cap.fbo);
        glBlitFramebuffer_(0, 0, w, h, 0, 0, w, h,
                           GL_COLOR_BUFFER_BIT, GL_NEAREST);
        // Restore DRAW to default — hardcoded 0, no query needed.
        glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, 0);
        cap.pending.store(true, std::memory_order_release);
    }
    // If FBO not ready: onScenePhase() will allocate it next frame.
}

static int drawCallbackPFD(XPLMDeviceID /*device*/,
                           int          /*isBefore*/,
                           void*        /*refcon*/) {
    if (!g_streamer) return 1;
    static bool once = false;
    if (!once) { XPLMDebugString("[X1000] PFD avionics callback fired\n"); once = true; }
    blitPanelToFBO(s_pfd_capture);
    return 1;
}

static int drawCallbackMFD(XPLMDeviceID /*device*/,
                           int          /*isBefore*/,
                           void*        /*refcon*/) {
    if (!g_streamer) return 1;
    static bool once = false;
    if (!once) { XPLMDebugString("[X1000] MFD avionics callback fired\n"); once = true; }
    blitPanelToFBO(s_mfd_capture);
    return 1;
}

// Window-phase callback — fires during 2-D overlay, after avionics are done.
// glReadPixels from FBO 0 here is safe under Zink/Vulkan.
static int scenePhaseCallback(XPLMDrawingPhase /*phase*/,
                              int              /*isBefore*/,
                              void*            /*refcon*/) {
    static bool once = false;
    if (!once) { XPLMDebugString("[X1000] scene-phase callback fired\n"); once = true; }
    if (g_streamer) g_streamer->onScenePhase();
    return 1;
}

// ---------------------------------------------------------------------------

DisplayStreamer::DisplayStreamer()
    : m_pfd_handle(nullptr)
    , m_mfd_handle(nullptr)
    , m_ready(false)
    , m_last_frame_time(0.0)
    , m_frame_seq(0)
    , m_pfd_sock(INVALID_SOCK)
    , m_mfd_sock(INVALID_SOCK)
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

static sock_t makeUDPSocket(const std::string& ip, uint16_t port,
                             sockaddr_in& out_addr) {
    // On Windows, socket() returns SOCKET (UINT_PTR, 64-bit).
    // Never cast to int — that truncates the handle on 64-bit Windows.
    sock_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCK) return INVALID_SOCK;
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

// Dummy draw callback used only for the hasG1000Avionics() probe.
static int drawCallbackProbe(XPLMDeviceID, int, void*) { return 1; }

bool DisplayStreamer::hasG1000Avionics() {
    // On X-Plane < 12.4.4, XPLMGetAvionicsHandle returns a valid handle
    // when a G1000 aircraft is loaded.  On 12.4.4+, it returns NULL until a
    // callback is registered first.  We try both paths so the check works on
    // all simulator versions.

    // Path 1: old API (12.4.3 and earlier)
    XPLMAvionicsID pfd = XPLMGetAvionicsHandle(xplm_device_G1000_PFD_1);
    XPLMAvionicsID mfd = XPLMGetAvionicsHandle(xplm_device_G1000_MFD);
    if (pfd && mfd) return true;

    // Path 2: new API (12.4.4+) — register a probe callback and check result
    XPLMCustomizeAvionics_t p = {};
    p.structSize        = sizeof(XPLMCustomizeAvionics_t);
    p.deviceId          = xplm_device_G1000_PFD_1;
    p.drawCallbackAfter = drawCallbackProbe;
#if defined(XPLM440)
    p.contentType       = xplm_WindowContentTypeOpenGL;
#endif
    XPLMAvionicsID probe = XPLMRegisterAvionicsCallbacksEx(&p);
    if (!probe) return false;            // not a G1000 aircraft
    XPLMUnregisterAvionicsCallbacks(probe);
    return true;
}

bool DisplayStreamer::acquireHandles() {
    // XPLMGetAvionicsHandle works on X-Plane < 12.4.4 without any prerequisites.
    // On 12.4.4+, it returns NULL until XPLMRegisterAvionicsCallbacksEx has been
    // called first (which init() does before calling acquireHandles).
    m_pfd_handle = XPLMGetAvionicsHandle(xplm_device_G1000_PFD_1);
    m_mfd_handle = XPLMGetAvionicsHandle(xplm_device_G1000_MFD);

    if (!m_pfd_handle || !m_mfd_handle) {
        // 12.4.4+ path: use callback handles as avionics handles.
        // XPLMGetAvionicsGeometry with these handles returns (0,0,0,0) on
        // some X-Plane 12.4.4 builds; if so, we fall back to the full viewport.
        if (m_pfd_cb_handle && m_mfd_cb_handle) {
            m_pfd_handle = m_pfd_cb_handle;
            m_mfd_handle = m_mfd_cb_handle;
            XPLMDebugString("[X1000] Using callback handles as avionics handles (X-Plane 12.4.4+)\n");
            return true;
        }
        XPLMDebugString("[X1000] XPLMGetAvionicsHandle returned NULL — no G1000 or callbacks not registered\n");
        return false;
    }

    // On 12.4.3 and earlier: confirm the G1000 is actually bound to this aircraft.
    // Limit attempts to avoid blocking the flight loop; the Plugin.cpp retry
    // loop (30 s backoff) handles repeated attempts.
    int pfd_bound = 0, mfd_bound = 0;
    for (int i = 0; i < 3; ++i) {
        pfd_bound = XPLMIsAvionicsBound(m_pfd_handle);
        mfd_bound = XPLMIsAvionicsBound(m_mfd_handle);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "[X1000] Avionics bound (attempt %d): PFD=%d MFD=%d\n",
                 i+1, pfd_bound, mfd_bound);
        XPLMDebugString(buf);
        if (pfd_bound && mfd_bound) break;
        usleep(100000);  // 100 ms — max 300 ms total block
    }
    if (!pfd_bound || !mfd_bound) {
        XPLMDebugString("[X1000] G1000 not bound — load a G1000 aircraft.\n");
        return false;
    }
    return true;
}

// Helper: register a draw callback, trying the new (XPLM440) struct first,
// then falling back to the old struct size for X-Plane < 12.4.4.
static XPLMAvionicsID registerCallback(XPLMDeviceID device,
                                        XPLMAvionicsCallback_f cb) {
    XPLMCustomizeAvionics_t p = {};
    p.structSize        = sizeof(XPLMCustomizeAvionics_t);
    p.deviceId          = device;
    p.drawCallbackAfter = cb;
#if defined(XPLM440)
    // Tell X-Plane we want to keep the legacy OpenGL framebuffer so that
    // glReadPixels / PBO capture continues to work on 12.4.4+.
    p.contentType       = xplm_WindowContentTypeOpenGL;
    XPLMAvionicsID h = XPLMRegisterAvionicsCallbacksEx(&p);
    if (h) return h;
    // Fall back to old struct size for X-Plane < 12.4.4 (ignores contentType)
    p.structSize  = (int)offsetof(XPLMCustomizeAvionics_t, contentType);
    p.contentType = 0;
    h = XPLMRegisterAvionicsCallbacksEx(&p);
    if (h) XPLMDebugString("[X1000] Using SDK 4.1.0 struct size (X-Plane < 12.4.4)\n");
    return h;
#else
    return XPLMRegisterAvionicsCallbacksEx(&p);
#endif
}

bool DisplayStreamer::registerDrawCallbacks() {
    m_pfd_cb_handle = registerCallback(xplm_device_G1000_PFD_1, drawCallbackPFD);
    if (!m_pfd_cb_handle) {
        XPLMDebugString("[X1000] Failed to register PFD draw callback\n");
        return false;
    }

    m_mfd_cb_handle = registerCallback(xplm_device_G1000_MFD, drawCallbackMFD);
    if (!m_mfd_cb_handle) {
        XPLMDebugString("[X1000] Failed to register MFD draw callback\n");
        return false;
    }

    XPLMDebugString("[X1000] Draw callbacks registered for PFD and MFD.\n");
    return true;
}

bool DisplayStreamer::init(const StreamConfig& cfg) {
    m_cfg = cfg;

    // Load GL extension function pointers needed for FBO operations.
    // Must be called before any glGenFramebuffers_ / glBindFramebuffer_ etc.
    if (!Platform::loadGLFBOFunctions()) {
        XPLMDebugString("[X1000] Failed to load GL FBO functions — capture disabled\n");
        return false;
    }

    // On X-Plane 12.4.4+, XPLMGetAvionicsHandle returns NULL until callbacks
    // have been registered.  Always register callbacks first, then acquire handles.
    if (!registerDrawCallbacks()) return false;
    if (!acquireHandles())        return false;

    m_pfd_sock = makeUDPSocket(cfg.pfd_ip, cfg.pfd_push_port, m_pfd_addr);
    m_mfd_sock = makeUDPSocket(cfg.mfd_ip, cfg.mfd_push_port, m_mfd_addr);

    if (m_pfd_sock == INVALID_SOCK || m_mfd_sock == INVALID_SOCK) {
        XPLMDebugString("[X1000] Failed to create UDP sockets\n");
        return false;
    }

    startWorker();

    // Register the scene-phase callback that does the actual glReadPixels.
    // xplm_Phase_Window fires during the 2-D overlay phase — guaranteed to
    // run on the render thread AFTER avionics FBOs have been rendered.
    // Reading from our own blit FBO here is safe under all GL backends.
    XPLMRegisterDrawCallback(scenePhaseCallback, xplm_Phase_Window,
                             1 /*isBefore=before*/, nullptr);
    m_scene_cb_registered = true;

    // Reset deferred capture state from any previous streamer instance.
    s_pfd_capture.pending.store(false, std::memory_order_relaxed);
    s_mfd_capture.pending.store(false, std::memory_order_relaxed);

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
// Avionics draw callbacks — only snapshot viewport, no GL readback.
// (glReadPixels inside an avionics cb crashes Zink/Vulkan; see deferred
//  capture comment above.)
// ---------------------------------------------------------------------------

void DisplayStreamer::onDrawPFD() {
    // Viewport snapshot is already done by the static drawCallbackPFD above.
    // Nothing else to do here; onScenePhase() handles the actual readback.
    (void)this;
}

void DisplayStreamer::onDrawMFD() {
    (void)this;
}

// ---------------------------------------------------------------------------
// onScenePhase — called during xplm_Phase_Window, after all avionics done.
//
// Fix 8: safe point for GL FBO allocation and glReadPixels.
// The blit (avionics FBO → our FBO) already happened in blitPanelToFBO()
// inside the avionics callback.  Here we just read from our own FBO.
// ---------------------------------------------------------------------------

// ensureBlitFBO — allocates/resizes our blit-destination FBO+texture.
// Called from onScenePhase() — safe for GL allocations (outside render pass).
static void ensureBlitFBO(DeferredCapture& cap) {
    GLint w = cap.rw, h = cap.rh;
    if (w <= 0 || h <= 0) return;

    // Check if already allocated at the right size
    if (cap.fbo_ready.load(std::memory_order_acquire)) return;

    // Allocate or reallocate
    if (cap.fbo) { glDeleteFramebuffers_(1, &cap.fbo); cap.fbo = 0; }
    if (cap.tex) { glDeleteTextures(1, &cap.tex);      cap.tex = 0; }

    glGenTextures(1, &cap.tex);
    glBindTexture(GL_TEXTURE_2D, cap.tex);
    // Use RGBA8 — some Mesa/Zink drivers silently reorder channels when reading
    // back an RGB8 texture with GL_RGB. RGBA8 + GL_RGBA readback is always safe.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers_(1, &cap.fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, cap.fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, cap.tex, 0);
    GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);

    if (status == GL_FRAMEBUFFER_COMPLETE) {
        cap.fbo_ready.store(true, std::memory_order_release);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "[X1000] BlitFBO allocated %dx%d fbo=%u tex=%u\n",
                 w, h, cap.fbo, cap.tex);
        XPLMDebugString(buf);
    } else {
        XPLMDebugString("[X1000] BlitFBO: FBO incomplete — capture disabled\n");
    }
}

void DisplayStreamer::onScenePhase() {
    if (!m_ready) return;

    // Allocate blit FBOs lazily once the panel dimensions are known.
    // Dimensions are written by blitPanelToFBO() in the avionics callback.
    if (s_pfd_capture.rw > 0 && !s_pfd_capture.fbo_ready.load(std::memory_order_relaxed))
        ensureBlitFBO(s_pfd_capture);
    if (s_mfd_capture.rw > 0 && !s_mfd_capture.fbo_ready.load(std::memory_order_relaxed))
        ensureBlitFBO(s_mfd_capture);

    double t = Platform::now_seconds();

    // PFD — read from our blit FBO (not from X-Plane's active framebuffer)
    if (m_pfd_sock != INVALID_SOCK &&
        s_pfd_capture.pending.load(std::memory_order_acquire) &&
        s_pfd_capture.fbo_ready.load(std::memory_order_acquire) &&
        (t - m_last_pfd_time) >= (1.0 / m_cfg.fps)) {
        s_pfd_capture.pending.store(false, std::memory_order_release);
        m_last_pfd_time = t;
        captureAndSend(0, 0, s_pfd_capture.rw, s_pfd_capture.rh,
                       s_pfd_capture.fbo,
                       m_pfd_sock, m_pfd_addr, "PFD");
    }

    // MFD
    if (m_mfd_sock != INVALID_SOCK &&
        s_mfd_capture.pending.load(std::memory_order_acquire) &&
        s_mfd_capture.fbo_ready.load(std::memory_order_acquire) &&
        (t - m_last_mfd_time) >= (1.0 / m_cfg.fps)) {
        s_mfd_capture.pending.store(false, std::memory_order_release);
        m_last_mfd_time = t;
        captureAndSend(0, 0, s_mfd_capture.rw, s_mfd_capture.rh,
                       s_mfd_capture.fbo,
                       m_mfd_sock, m_mfd_addr, "MFD");
    }
}

// ---------------------------------------------------------------------------
// captureAndSend — glReadPixels from our own blit FBO
//
// Called from onScenePhase() — safe for GL state operations (outside any
// active Vulkan render pass).  We bind our own FBO (populated by
// blitPanelToFBO in the avionics callback) as READ, read pixels from (0,0),
// then restore READ to 0.  No glGetIntegerv — restoration is hardcoded.
// ---------------------------------------------------------------------------

void DisplayStreamer::captureAndSend(int rx, int ry, int rw, int rh,
                                     GLuint fbo,
                                     sock_t sock, sockaddr_in& addr,
                                     const char* name) {
    rw &= ~1; rh &= ~1;
    if (rw <= 0 || rh <= 0 || !fbo) return;

    // Log first capture dimensions once per display
    static bool pfd_logged = false, mfd_logged = false;
    bool& logged = (name[0] == 'P') ? pfd_logged : mfd_logged;

    const int TARGET_W = m_cfg.stream_width & ~1;
    const int TARGET_H = (rh * TARGET_W / rw) & ~1;
    if (TARGET_W <= 0 || TARGET_H <= 0) return;

    if (!logged) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[X1000] %s first frame: %dx%d -> %dx%d (fbo=%u)\n",
                 name, rw, rh, TARGET_W, TARGET_H, fbo);
        XPLMDebugString(buf);
        logged = true;
    }

    // Bind our blit FBO as READ source and read pixels from (0,0).
    // Our FBO is not part of any X-Plane render pass — safe under Zink.
    // Restore READ to 0 hardcoded (no glGetIntegerv needed).
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, fbo);
    // Read as RGBA — avoids channel-order issues with RGB8 textures under Mesa/Zink.
    // Alpha channel is discarded in the scale loop below.
    std::vector<uint8_t> raw(rw * rh * 4);
    glReadPixels(rx, ry, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glBindFramebuffer_(GL_READ_FRAMEBUFFER, 0);

    // Read display brightness
    static XPLMDataRef s_brightness_dr = nullptr;
    if (!s_brightness_dr)
        s_brightness_dr = XPLMFindDataRef("sim/cockpit2/electrical/display_screen_brightness");
    float brightness = 1.0f;
    if (s_brightness_dr) {
        float vals[8] = {1,1,1,1,1,1,1,1};
        XPLMGetDatavf(s_brightness_dr, vals, 0, 8);
        brightness = (name[0] == 'P') ? vals[0] : vals[2];
        if (brightness < 0.1f) brightness = 0.1f;
        if (brightness > 1.0f) brightness = 1.0f;
    }

    // Scale down + vertical flip (GL origin is bottom-left; display expects top-left)
    // + apply brightness — all in one pass into the worker job buffer.
    // Scale + vertical flip + brightness, RGBA src → RGB dst (drop alpha).
    std::vector<uint8_t> scaled(TARGET_W * TARGET_H * 3);
    float scaleX = (float)rw / TARGET_W;
    float scaleY = (float)rh / TARGET_H;
    for (int dy = 0; dy < TARGET_H; ++dy) {
        int sy = (int)((TARGET_H - 1 - dy) * scaleY);
        if (sy >= rh) sy = rh - 1;
        const uint8_t* src_row = &raw[sy * rw * 4];   // stride 4 (RGBA)
        uint8_t*       dst_row = &scaled[dy * TARGET_W * 3];
        for (int dx = 0; dx < TARGET_W; ++dx) {
            int sx = (int)(dx * scaleX);
            if (sx >= rw) sx = rw - 1;
            const uint8_t* src = src_row + sx * 4;    // RGBA pixel
            uint8_t*       dst = dst_row + dx * 3;    // RGB pixel
            dst[0] = (uint8_t)(src[0] * brightness);  // R
            dst[1] = (uint8_t)(src[1] * brightness);  // G
            dst[2] = (uint8_t)(src[2] * brightness);  // B  (src[3] = A, discarded)
        }
    }

    // Hand off to background worker for JPEG encode + UDP send.
    // Drop frame if queue is backing up (keep latency low).
    {
        std::unique_lock<std::mutex> lock(s_queue_mutex);
        if (s_encode_queue.size() < 2) {
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
        // else: drop this frame — sim responsiveness takes priority
    }
}

// ---------------------------------------------------------------------------
// UDP push with chunking
// ---------------------------------------------------------------------------

void DisplayStreamer::pushFrame(const uint8_t* data, size_t len,
                                sock_t sock, sockaddr_in& addr) {
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
    m_ready = false;   // stop onScenePhase() from doing work first

    if (m_scene_cb_registered) {
        XPLMUnregisterDrawCallback(scenePhaseCallback, xplm_Phase_Window,
                                   1 /*isBefore=before*/, nullptr);
        m_scene_cb_registered = false;
    }
    if (m_pfd_cb_handle) {
        XPLMUnregisterAvionicsCallbacks(m_pfd_cb_handle);
        m_pfd_cb_handle = nullptr;
    }
    if (m_mfd_cb_handle) {
        XPLMUnregisterAvionicsCallbacks(m_mfd_cb_handle);
        m_mfd_cb_handle = nullptr;
    }
    stopWorker();

    // Reset pending flags before freeing GL resources.
    s_pfd_capture.pending.store(false, std::memory_order_relaxed);
    s_mfd_capture.pending.store(false, std::memory_order_relaxed);

    // Free blit FBOs (called on render thread — GL context is current).
    auto freeFBO = [](DeferredCapture& cap) {
        cap.fbo_ready.store(false, std::memory_order_relaxed);
        if (cap.fbo) { glDeleteFramebuffers_(1, &cap.fbo); cap.fbo = 0; }
        if (cap.tex) { glDeleteTextures(1, &cap.tex);      cap.tex = 0; }
        cap.rw = cap.rh = 0;
    };
    freeFBO(s_pfd_capture);
    freeFBO(s_mfd_capture);

    // close() is #define'd to closesocket() on Windows; works on both platforms.
    if (m_pfd_sock != INVALID_SOCK) { close(m_pfd_sock); m_pfd_sock = INVALID_SOCK; }
    if (m_mfd_sock != INVALID_SOCK) { close(m_mfd_sock); m_mfd_sock = INVALID_SOCK; }
    m_pfd_handle = nullptr;
    m_mfd_handle = nullptr;
    XPLMDebugString("[X1000] DisplayStreamer: shutdown.\n");
}
