#pragma once
// Platform.h — Cross-platform abstractions for X1000_display
//
// Supported platforms:
//   LIN  — Linux (Ubuntu 24.04+)     — defined by compile.sh: -DLIN=1
//   _WIN32                            — Windows (MinGW-w64 or MSVC)
//   __APPLE__                         — macOS (clang, x64 or arm64)
//
// Usage:
//   #include "Platform.h"
//   Platform::sleep_ms(100);
//   auto ip = Platform::detectLocalIP();
//   auto pid = Platform::spawnProcess("python3", {script, "--port", "9000"});
//   Platform::loadGL();   // then use Platform::glGenBuffers(...)

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ---------------------------------------------------------------------------
// GL types needed before platform-specific GL include
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
#  include <OpenGL/gl.h>
#elif defined(_WIN32)
#  include <winsock2.h>
#  include <windows.h>
#  include <GL/gl.h>
#else
#  include <GL/gl.h>
#endif

// ---------------------------------------------------------------------------
// Process handle — opaque across platforms
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  include <winsock2.h>
#  include <windows.h>
   using PlatformPID = HANDLE;
#  define INVALID_PID ((PlatformPID)NULL)
#else
#  include <sys/types.h>
   using PlatformPID = pid_t;
#  define INVALID_PID ((PlatformPID)-1)
#endif

namespace Platform {

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

void sleep_ms(int ms);
void sleep_us(int us);

// ---------------------------------------------------------------------------
// Network — local IP detection
// ---------------------------------------------------------------------------

// Returns best LAN IP (prefers 192.168.x.x, then 10.x.x.x, then any non-lo)
std::string detectLocalIP();

// ---------------------------------------------------------------------------
// Process management — spawn a child process with stdout/stderr pipe
// ---------------------------------------------------------------------------

struct ProcessHandle {
    PlatformPID pid        = INVALID_PID;
    int         stdout_fd  = -1;   // read end of stdout+stderr pipe (-1 on Windows = use win_pipe)
#if defined(_WIN32)
    HANDLE      win_pipe   = INVALID_HANDLE_VALUE;
#endif
    bool isValid() const { return pid != INVALID_PID; }
};

// Spawn process with args. stdout+stderr captured via pipe.
// args[0] is the executable, args[1..] are arguments.
ProcessHandle spawnProcess(const std::vector<std::string>& args);

// Send SIGTERM / TerminateProcess and wait for exit
void killProcess(ProcessHandle& handle);

// Non-blocking check if process is still alive
bool isProcessAlive(const ProcessHandle& handle);

// Read available output from pipe (non-blocking, returns "" if nothing)
std::string readProcessOutput(const ProcessHandle& handle, int timeout_ms = 200);

// ---------------------------------------------------------------------------
// GLsizeiptr may not be defined in basic Windows GL headers — define if needed
#if defined(_WIN32) && !defined(GL_VERSION_1_5)
   typedef ptrdiff_t GLsizeiptr;
   typedef ptrdiff_t GLintptr;
#endif

// ---------------------------------------------------------------------------
// GL buffer object functions (OpenGL 1.5 — loaded at runtime)
// ---------------------------------------------------------------------------

// Call once before using any GL* functions below.
// Returns true if all functions loaded successfully.
bool loadGLBufferFunctions();

// These mirror the standard GL 1.5 VBO/PBO API.
// Populated by loadGLBufferFunctions().
extern void      (*glGenBuffers_)   (GLsizei, GLuint*);
extern void      (*glDeleteBuffers_)(GLsizei, const GLuint*);
extern void      (*glBindBuffer_)   (GLenum, GLuint);
extern void      (*glBufferData_)   (GLenum, GLsizeiptr, const void*, GLenum);
extern void*     (*glMapBuffer_)    (GLenum, GLenum);
extern GLboolean (*glUnmapBuffer_)  (GLenum);

// Convenience macros so call sites look like standard GL
#define glGenBuffers    Platform::glGenBuffers_
#define glDeleteBuffers Platform::glDeleteBuffers_
#define glBindBuffer    Platform::glBindBuffer_
#define glBufferData    Platform::glBufferData_
#define glMapBuffer     Platform::glMapBuffer_
#define glUnmapBuffer   Platform::glUnmapBuffer_

// ---------------------------------------------------------------------------
// Path utilities
// ---------------------------------------------------------------------------

// Platform path separator ('/' on Linux/Mac, '\\' on Windows)
char pathSeparator();

// Join two path components with the correct separator
std::string pathJoin(const std::string& a, const std::string& b);

// Normalise path separators for current platform
std::string normalisePath(const std::string& path);

// Name of python executable ('python3' on Linux/Mac, 'python.exe' on Windows)
std::string pythonExecutable();

// Monotonic time in seconds — cross-platform replacement for clock_gettime
double now_seconds();

} // namespace Platform

// ---------------------------------------------------------------------------
// GL extension constants (may not be in base gl.h on all platforms)
// ---------------------------------------------------------------------------

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_READ_ONLY
#define GL_READ_ONLY 0x88B8
#endif
