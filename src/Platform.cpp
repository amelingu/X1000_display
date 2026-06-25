// Platform.cpp — Cross-platform implementations

#include "Platform.h"
#include <XPLMUtilities.h>
#include <cstring>
#include <cstdio>

// ============================================================================
// LINUX / macOS shared POSIX implementation
// ============================================================================
#if defined(LIN) || defined(__APPLE__)

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <dlfcn.h>
#include <errno.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

void Platform::sleep_ms(int ms) { usleep(ms * 1000); }
void Platform::sleep_us(int us) { usleep(us); }

// ---------------------------------------------------------------------------
// IP detection
// ---------------------------------------------------------------------------

std::string Platform::detectLocalIP() {
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return "127.0.0.1";

    std::string best;
    int best_score = -1;

    for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        auto* sa = (struct sockaddr_in*)ifa->ifa_addr;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
        std::string ip(buf);
        if (ip == "127.0.0.1") continue;

        int score = 0;
        if      (ip.substr(0,8) == "192.168.") score = 3;
        else if (ip.substr(0,3) == "10.")       score = 2;
        else if (ip.substr(0,7) == "172.16."
              || ip.substr(0,7) == "172.17.") score = 1;

        if (score > best_score) { best_score = score; best = ip; }
    }
    freeifaddrs(ifap);
    return best.empty() ? "127.0.0.1" : best;
}

// ---------------------------------------------------------------------------
// Process management
// ---------------------------------------------------------------------------

Platform::ProcessHandle Platform::spawnProcess(const std::vector<std::string>& args) {
    ProcessHandle h;
    if (args.empty()) return h;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        XPLMDebugString("[X1000] Platform::spawnProcess: pipe() failed\n");
        return h;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if (pid < 0) {
        XPLMDebugString("[X1000] Platform::spawnProcess: fork() failed\n");
        close(pipefd[0]); close(pipefd[1]);
        return h;
    }

    if (pid == 0) {
        // Child
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);

        // Build argv
        std::vector<char*> argv;
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        _exit(1);
    }

    // Parent
    close(pipefd[1]);
    h.pid       = pid;
    h.stdout_fd = pipefd[0];
    return h;
}

void Platform::killProcess(ProcessHandle& handle) {
    if (handle.pid <= 0) return;
    kill(handle.pid, SIGTERM);
    int status;
    for (int i = 0; i < 20; ++i) {
        if (waitpid(handle.pid, &status, WNOHANG) == handle.pid) goto done;
        usleep(100000);
    }
    kill(handle.pid, SIGKILL);
    waitpid(handle.pid, &status, 0);
done:
    handle.pid = INVALID_PID;
    if (handle.stdout_fd >= 0) { close(handle.stdout_fd); handle.stdout_fd = -1; }
}

bool Platform::isProcessAlive(const ProcessHandle& handle) {
    if (handle.pid <= 0) return false;
    return waitpid(handle.pid, nullptr, WNOHANG) == 0;
}

std::string Platform::readProcessOutput(const ProcessHandle& handle, int timeout_ms) {
    if (handle.stdout_fd < 0) return "";
    struct pollfd pfd = { handle.stdout_fd, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_ms) <= 0) return "";
    char buf[1024];
    ssize_t n = read(handle.stdout_fd, buf, sizeof(buf)-1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return std::string(buf, n);
}

// ---------------------------------------------------------------------------
// GL buffer functions
// ---------------------------------------------------------------------------

void      (*Platform::glGenBuffers_)   (GLsizei, GLuint*)              = nullptr;
void      (*Platform::glDeleteBuffers_)(GLsizei, const GLuint*)        = nullptr;
void      (*Platform::glBindBuffer_)   (GLenum, GLuint)                = nullptr;
void      (*Platform::glBufferData_)   (GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void*     (*Platform::glMapBuffer_)    (GLenum, GLenum)                = nullptr;
GLboolean (*Platform::glUnmapBuffer_)  (GLenum)                        = nullptr;

bool Platform::loadGLBufferFunctions() {
    static bool loaded = false, ok = false;
    if (loaded) return ok;
    loaded = true;

#if defined(__APPLE__)
    const char* libname = "/System/Library/Frameworks/OpenGL.framework/OpenGL";
#else
    const char* libname = "libGL.so.1";
#endif
    void* lib = dlopen(libname, RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = dlopen(libname, RTLD_NOW);
#if defined(LIN)
    if (!lib) lib = dlopen("libGL.so", RTLD_NOW);
#endif
    if (!lib) {
        XPLMDebugString("[X1000] Platform: could not open GL library\n");
        return false;
    }

    glGenBuffers_    = (void(*)(GLsizei,GLuint*))               dlsym(lib,"glGenBuffers");
    glDeleteBuffers_ = (void(*)(GLsizei,const GLuint*))         dlsym(lib,"glDeleteBuffers");
    glBindBuffer_    = (void(*)(GLenum,GLuint))                 dlsym(lib,"glBindBuffer");
    glBufferData_    = (void(*)(GLenum,GLsizeiptr,const void*,GLenum)) dlsym(lib,"glBufferData");
    glMapBuffer_     = (void*(*)(GLenum,GLenum))                dlsym(lib,"glMapBuffer");
    glUnmapBuffer_   = (GLboolean(*)(GLenum))                   dlsym(lib,"glUnmapBuffer");

    ok = glGenBuffers_ && glDeleteBuffers_ && glBindBuffer_
      && glBufferData_ && glMapBuffer_    && glUnmapBuffer_;

    XPLMDebugString(ok ? "[X1000] Platform: GL buffer functions loaded OK\n"
                       : "[X1000] Platform: some GL functions missing\n");
    return ok;
}

// ---------------------------------------------------------------------------
// Path utilities
// ---------------------------------------------------------------------------

char        Platform::pathSeparator()  { return '/'; }
std::string Platform::pythonExecutable() { return "python3"; }

std::string Platform::pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + '/' + b;
}

std::string Platform::normalisePath(const std::string& path) { return path; }

double Platform::now_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// ============================================================================
// WINDOWS implementation
// ============================================================================
#elif defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

void Platform::sleep_ms(int ms) { Sleep(ms); }
void Platform::sleep_us(int us) { Sleep(us / 1000 > 0 ? us / 1000 : 1); }

std::string Platform::detectLocalIP() {
    ULONG buf_size = 15000;
    std::vector<char> buf(buf_size);
    auto* adapters = (IP_ADAPTER_INFO*)buf.data();

    if (GetAdaptersInfo(adapters, &buf_size) != ERROR_SUCCESS) return "127.0.0.1";

    std::string best;
    int best_score = -1;

    for (auto* a = adapters; a; a = a->Next) {
        std::string ip = a->IpAddressList.IpAddress.String;
        if (ip == "0.0.0.0" || ip == "127.0.0.1") continue;

        int score = 0;
        if      (ip.substr(0,8) == "192.168.") score = 3;
        else if (ip.substr(0,3) == "10.")       score = 2;
        else if (ip.substr(0,7) == "172.16.")   score = 1;

        if (score > best_score) { best_score = score; best = ip; }
    }
    return best.empty() ? "127.0.0.1" : best;
}

Platform::ProcessHandle Platform::spawnProcess(const std::vector<std::string>& args) {
    ProcessHandle h;
    if (args.empty()) return h;

    // Build command line string
    std::string cmdline;
    for (const auto& a : args) {
        if (!cmdline.empty()) cmdline += ' ';
        // Quote args with spaces
        bool needs_quote = a.find(' ') != std::string::npos;
        if (needs_quote) cmdline += '"';
        cmdline += a;
        if (needs_quote) cmdline += '"';
    }

    // Create pipe for stdout+stderr
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE pipe_read, pipe_write;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return h;
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = pipe_write;
    si.hStdError   = pipe_write;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, const_cast<char*>(cmdline.c_str()),
                        nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pipe_read); CloseHandle(pipe_write);
        return h;
    }

    CloseHandle(pipe_write);
    CloseHandle(pi.hThread);

    h.pid      = pi.hProcess;
    h.win_pipe = pipe_read;
    return h;
}

void Platform::killProcess(ProcessHandle& handle) {
    if (handle.pid == INVALID_HANDLE_VALUE) return;
    TerminateProcess(handle.pid, 0);
    WaitForSingleObject(handle.pid, 3000);
    CloseHandle(handle.pid);
    if (handle.win_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(handle.win_pipe);
        handle.win_pipe = INVALID_HANDLE_VALUE;
    }
    handle.pid = INVALID_HANDLE_VALUE;
}

bool Platform::isProcessAlive(const ProcessHandle& handle) {
    if (handle.pid == INVALID_HANDLE_VALUE) return false;
    DWORD exit_code;
    if (!GetExitCodeProcess(handle.pid, &exit_code)) return false;
    return exit_code == STILL_ACTIVE;
}

std::string Platform::readProcessOutput(const ProcessHandle& handle, int /*timeout_ms*/) {
    if (handle.win_pipe == INVALID_HANDLE_VALUE) return "";
    DWORD avail = 0;
    if (!PeekNamedPipe(handle.win_pipe, nullptr, 0, nullptr, &avail, nullptr)
        || avail == 0) return "";
    std::vector<char> buf(avail + 1, 0);
    DWORD read = 0;
    ReadFile(handle.win_pipe, buf.data(), avail, &read, nullptr);
    return std::string(buf.data(), read);
}

// GL functions — use wglGetProcAddress on Windows
void      (*Platform::glGenBuffers_)   (GLsizei, GLuint*)              = nullptr;
void      (*Platform::glDeleteBuffers_)(GLsizei, const GLuint*)        = nullptr;
void      (*Platform::glBindBuffer_)   (GLenum, GLuint)                = nullptr;
void      (*Platform::glBufferData_)   (GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void*     (*Platform::glMapBuffer_)    (GLenum, GLenum)                = nullptr;
GLboolean (*Platform::glUnmapBuffer_)  (GLenum)                        = nullptr;

bool Platform::loadGLBufferFunctions() {
    static bool loaded = false, ok = false;
    if (loaded) return ok;
    loaded = true;

    // Cast via void* to avoid MinGW strict-cast warnings
    void* p;
    p = (void*)wglGetProcAddress("glGenBuffers");
    memcpy(&glGenBuffers_,    &p, sizeof(p));
    p = (void*)wglGetProcAddress("glDeleteBuffers");
    memcpy(&glDeleteBuffers_, &p, sizeof(p));
    p = (void*)wglGetProcAddress("glBindBuffer");
    memcpy(&glBindBuffer_,    &p, sizeof(p));
    p = (void*)wglGetProcAddress("glBufferData");
    memcpy(&glBufferData_,    &p, sizeof(p));
    p = (void*)wglGetProcAddress("glMapBuffer");
    memcpy(&glMapBuffer_,     &p, sizeof(p));
    p = (void*)wglGetProcAddress("glUnmapBuffer");
    memcpy(&glUnmapBuffer_,   &p, sizeof(p));

    ok = glGenBuffers_ && glDeleteBuffers_ && glBindBuffer_
      && glBufferData_ && glMapBuffer_    && glUnmapBuffer_;

    XPLMDebugString(ok ? "[X1000] Platform: GL buffer functions loaded OK\n"
                       : "[X1000] Platform: some GL functions missing\n");
    return ok;
}

char        Platform::pathSeparator()   { return '\\'; }
std::string Platform::pythonExecutable() { return "python3.exe"; }

std::string Platform::pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char sep = a.back();
    if (sep == '/' || sep == '\\') return a + b;
    return a + '\\' + b;
}

std::string Platform::normalisePath(const std::string& path) {
    std::string r = path;
    for (char& c : r) if (c == '/') c = '\\';
    return r;
}

double Platform::now_seconds() {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
}

#endif // platform
