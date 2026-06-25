#pragma once
#include "SettingsManager.h"
#include "RelayManager.h"
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <string>
#include <functional>

enum class ClickAction;
using ApplyCallback = std::function<void()>;

class UIManager {
public:
    UIManager();
    ~UIManager();

    void init(SettingsManager* settings, RelayManager* relay, ApplyCallback on_apply);
    void show(); void hide(); void toggle();
    bool isVisible() const;
    void tick(bool pfd_streaming, bool mfd_streaming,
              int pfd_fps, int mfd_fps, int pfd_kb, int mfd_kb);

private:
    static void drawCB(XPLMWindowID, void* ref);
    static int  mouseCB(XPLMWindowID, int x, int y, XPLMMouseStatus, void* ref);
    static void keyboardCB(XPLMWindowID, char key, XPLMKeyFlags, char, void* ref, int);
    static XPLMCursorStatus cursorCB(XPLMWindowID, int, int, void*);
    static void menuCB(void*, void* item_ref);

    void draw();
    void drawStatus(int& y, int left, int right);
    void drawNetwork(int& y, int left, int right);
    void drawSetupGuide(int& y, int left, int right);
    void drawRelayLog(int& y, int left, int right);
    void drawAbout(int& y, int left, int right);
    void drawSection(const char* title, int& y, int left, int right);

    void drawText(int x, int y, const char* text, float r=1,float g=1,float b=1);
    void drawRect(int x1,int y1,int x2,int y2, float r,float g,float b,float a);
    void drawButton(int x,int y,int w,int h, const char* label,
                    ClickAction action,
                    float r=0.2f,float g=0.2f,float b=0.3f);
    void drawTextInput(int x,int y,int w, char* buf,int bufsz,
                       bool active, ClickAction action);
    void drawCheckbox(int x,int y, const char* label, bool& value,
                      ClickAction action);

    void handleClick(int x, int y);

    SettingsManager* m_settings = nullptr;
    RelayManager*    m_relay    = nullptr;
    ApplyCallback    m_on_apply;

    XPLMWindowID m_window   = nullptr;
    XPLMMenuID   m_menu     = nullptr;

    static constexpr int WIN_W = 490;
    static constexpr int WIN_H = 600;

    bool m_pfd_streaming = false, m_mfd_streaming = false;
    int  m_pfd_fps = 0, m_mfd_fps = 0;
    int  m_pfd_kb  = 0, m_mfd_kb  = 0;

    char m_buf_pfd_port[8]  = "9000";
    char m_buf_mfd_port[8]  = "9001";
    char m_buf_width[8]     = "1024";
    char m_buf_quality[8]   = "85";
    char m_buf_fps[8]       = "15";
    char m_buf_pc_ip[32]    = "";
    int  m_active_field     = -1;

    int  m_log_scroll       = 0;
    bool m_show_log         = false;
};
