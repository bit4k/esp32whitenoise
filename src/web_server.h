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

private:
    bool otaRequested;
};

extern WebServerManager webServer;
