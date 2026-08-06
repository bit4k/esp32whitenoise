#include <Arduino.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include "storage.h"
#include "noise_gen.h"
#include "bt_audio.h"
#include "web_server.h"
#include <Ticker.h>

WiFiManager wm;
Ticker ledTicker;
const int LED_PIN = 5; // Wemos LOLIN32 (mit Battery Connector) nutzt oft Pin 5

void toggleLED() {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
}

// Timer and Disconnect logic
bool timerExpired = false;
uint32_t expireTime = 0;

void setup() {
    Serial.begin(115200);
    
    // Init LED
    pinMode(LED_PIN, OUTPUT);
    ledTicker.attach(1.0, toggleLED); // Slow blink (1s) initially
    
    // Init Storage
    storage.begin();
    esp_log_level_set("BT_AV", ESP_LOG_WARN);
    esp_log_level_set("BT_APP", ESP_LOG_WARN);

    
    std::vector<String> devs = storage.getSavedDevices();
    
    // Init SPIFFS
    if(!SPIFFS.begin(true)){
        Serial.println("SPIFFS Mount Failed");
        return;
    }
    
    // Load last noise type
    int type = storage.getLastNoiseType();
    noiseGen.setType(type);
    
    // Init WiFiManager
    // wm.resetSettings(); // for debugging
    WiFi.setHostname("white-noise");
    bool res = wm.autoConnect("ESP32_WhiteNoise_Setup");
    if(!res) {
        Serial.println("Failed to connect");
        // ESP.restart();
    } else {
        Serial.println("connected to wifi)");
        
        // Start Web Server first
        webServer.begin();
        
        // Start mDNS
        if (MDNS.begin("white-noise")) {
            Serial.println("mDNS responder started: http://white-noise.local");
        }
        
        // Check for updates automatically
        webServer.autoCheckOTA();
    }
    
    // Init BT Audio LAST to ensure WiFi/OTA/Webserver have enough memory to initialize
    btAudio.begin(devs);
    
    if (devs.size() > 0) {
        // Play welcome/current noise track announcement after a short delay to let connection establish
        delay(3000);
        String initialMsg = String("/") + String(type) + ".mp3";
        btAudio.playAnnouncement(initialMsg.c_str());
    }
}

void loop() {
    // Process async tasks
    webServer.loop();
    btAudio.loop();
    
    // Timer Logic
    static bool wasConnected = false;
    bool isConn = btAudio.isConnected();
    if (isConn != wasConnected) {
        wasConnected = isConn;
        if (isConn) {
            Serial.println("\n>>> BLUETOOTH-LAUTSPRECHER VERBUNDEN! <<<\n");
            ledTicker.attach(0.2, toggleLED); // Fast blink when streaming
        } else {
            Serial.println("\n>>> BLUETOOTH-LAUTSPRECHER GETRENNT! <<<\n");
            ledTicker.attach(1.0, toggleLED); // Slow blink when disconnected
        }
    }

    if (btAudio.getTimerState() != TIMER_ENDLESS) {
        uint32_t duration = (btAudio.getTimerState() == TIMER_30_MIN) ? (30 * 60 * 1000) : (60 * 60 * 1000);
        uint32_t elapsed = millis() - btAudio.getTimerStartTime();
        
        if (!timerExpired && elapsed > duration) {
            timerExpired = true;
            expireTime = millis();
            // To mute audio, we can just switch to an invalid noise type, 
            // but we didn't implement that. We can add a mute flag, or disconnect immediately.
            // For now, let's just wait 5 minutes then disconnect. The silence might need an explicit mute.
        }
        
        if (timerExpired) {
            // Check if 5 minutes have passed since expire
            if (millis() - expireTime > 5 * 60 * 1000) {
                if (btAudio.isConnected()) {
                    btAudio.disconnect();
                    Serial.println("Timer expired, 5 mins passed -> Disconnected BT.");
                }
            }
        }
    } else {
        timerExpired = false;
    }
}
