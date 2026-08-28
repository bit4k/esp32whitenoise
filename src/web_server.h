#pragma once

#include <Arduino.h>

class WebServerManager {
public:
    WebServerManager();
    void begin();
    void loop();
    
    // OTA Update Trigger
    void triggerOTA();
    
    // Automatic OTA Check
    void autoCheckOTA();

    // Deferred Connect & Restart
    void scheduleConnect(const String& name);

private:
    bool otaRequested;
    bool connectRequested;
    String targetDeviceName;
    uint32_t pendingRestartTime;
};

extern WebServerManager webServer;
