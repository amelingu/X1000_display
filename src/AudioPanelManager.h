#pragma once
// AudioPanelManager.h — G1000 audio panel button dispatch via X-Plane commands

class AudioPanelManager {
public:
    AudioPanelManager();
    void init();
    void handleUKP(unsigned int ukp);

private:
    bool m_ready = false;
};
