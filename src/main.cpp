#include <Arduino.h>
#include <WiFiManager.h>
#include <SPIFFS.h>
#include <ESPmDNS.h>
#include "storage.h"
#include "noise_gen.h"
#include "bt_audio.h"
#include "web_server.h"
#include <Ticker.h>
#include <esp_gap_bt_api.h>

WiFiManager wm;
const int LED_PIN = 5; // Wemos LOLIN32 / ESP32 LED Pin

void handleLED() {
    static uint32_t lastCycle = 0;
    static bool ledActive = false;
    uint32_t now = millis();
    bool isConn = btAudio.isConnected();

    if (isConn) {
        // Bluetooth Connected: Ultra-short 15ms flash once every 3 seconds (active LOW)
        if (!ledActive && (now - lastCycle >= 3000)) {
            lastCycle = now;
            ledActive = true;
            digitalWrite(LED_PIN, LOW); // ON
        } else if (ledActive && (now - lastCycle >= 15)) {
            ledActive = false;
            digitalWrite(LED_PIN, HIGH); // OFF
        }
    } else {
        // Disconnected / No Bluetooth: LED ALWAYS OFF (Active LOW -> HIGH is OFF)
        ledActive = false;
        digitalWrite(LED_PIN, HIGH);
    }
}

// Timer and Disconnect logic
bool timerExpired = false;
uint32_t expireTime = 0;
bool reconnectPending = false;
uint32_t disconnectTime = 0;

void setup() {
    Serial.begin(115200);
    
    // Init LED (Active LOW -> HIGH is OFF)
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    
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
    
    // Register AP Callback to print SoftAP IP & details when setup portal starts
    wm.setAPCallback([](WiFiManager *myWiFiManager) {
        Serial.println("\n============================================");
        Serial.println("[WiFiManager] SoftAP Config Portal Started!");
        Serial.printf("[WiFiManager] AP SSID: %s\n", myWiFiManager->getConfigPortalSSID().c_str());
        Serial.printf("[WiFiManager] AP IP Address: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.println("============================================\n");
    });
    
    bool res = wm.autoConnect("ESP32_WhiteNoise_Setup");
    if(!res) {
        Serial.println("\n[WiFiManager] Failed to connect or timeout reached.");
    } else {
        Serial.println("\n============================================");
        Serial.println("[WiFi] Connected to WiFi!");
        Serial.printf("[WiFi] STA IP Address: %s\n", WiFi.localIP().toString().c_str());
        Serial.println("============================================\n");
        
        // Start Web Server
        webServer.begin();
        
        // Start mDNS
        if (MDNS.begin("white-noise")) {
            Serial.println("[mDNS] Responder started: http://white-noise.local");
        }
    }
    
    // Init BT Audio LAST to ensure WiFi/OTA/Webserver have enough memory to initialize
    btAudio.begin(devs);
}

void loop() {
    // Process async tasks
    webServer.loop();
    btAudio.loop();
    handleLED();
    
    // Timer Logic
    static bool wasConnected = false;
    static bool wasPaused = false;
    bool isConn = btAudio.isConnected();
    bool isPaused = btAudio.isPaused;
    
    if (isConn != wasConnected || (isConn && isPaused != wasPaused)) {
        if (isConn != wasConnected) {
            if (isConn) {
                Serial.println("\n>>> BLUETOOTH-LAUTSPRECHER VERBUNDEN! <<<\n");
            } else {
                Serial.println("\n>>> BLUETOOTH-LAUTSPRECHER GETRENNT! <<<\n");
            }
        }
        wasConnected = isConn;
        wasPaused = isPaused;
    }

    if (btAudio.getTimerState() != TIMER_ENDLESS) {
        uint32_t duration = (btAudio.getTimerState() == TIMER_30_MIN) ? (30 * 60 * 1000) : (60 * 60 * 1000);
        uint32_t fadeDuration = 2 * 60 * 1000; // 2 minutes (120,000 ms)
        uint32_t fadeStart = (duration > fadeDuration) ? (duration - fadeDuration) : 0;
        uint32_t elapsed = millis() - btAudio.getTimerStartTime();
        
        static bool warningBeepTriggered = false;
        
        if (elapsed >= fadeStart && elapsed < duration) {
            float fadeFactor = (float)(duration - elapsed) / (float)fadeDuration;
            if (fadeFactor < 0.0f) fadeFactor = 0.0f;
            btAudio.setFadeFactor(fadeFactor);
            
            if (!warningBeepTriggered) {
                warningBeepTriggered = true;
                btAudio.triggerWarningBeep();
                Serial.println("\n[Timer] 2 Minuten vor Ende: Piepton abgespielt & Sanftes Ausblenden gestartet.\n");
            }
        } else if (elapsed < fadeStart) {
            btAudio.setFadeFactor(1.0f);
            warningBeepTriggered = false;
        } else {
            btAudio.setFadeFactor(0.0f);
            warningBeepTriggered = false;
        }
        
        if (!timerExpired && elapsed >= duration) {
            timerExpired = true;
            expireTime = millis();
            Serial.println("\n[Timer] Sleep Timer abgelaufen (Lautstärke auf 0). Trenne BT in 5 Minuten.\n");
        }
        
        if (timerExpired) {
            // Check if 5 minutes have passed since expire
            if (millis() - expireTime > 5 * 60 * 1000) {
                if (btAudio.isConnected()) {
                    btAudio.disconnect();
                    Serial.println("Timer expired, 5 mins passed -> Disconnected BT to allow speaker to power off.");
                    reconnectPending = true;
                    disconnectTime = millis();
                }
            }
        }
    } else {
        timerExpired = false;
        reconnectPending = false;
        btAudio.setFadeFactor(1.0f);
    }
    
    // If we disconnected due to sleep timer, wait 30 minutes before allowing reconnections.
    // This gives the speaker enough time to execute its own auto-power-off (usually 10-15 mins).
    if (reconnectPending && (millis() - disconnectTime > 30 * 60 * 1000)) {
        reconnectPending = false;
        timerExpired = false; // MUST reset this so it doesn't immediately disconnect again!
        btAudio.resetTimer(); // Reset the timer so it plays music when it reconnects
        Serial.println("30 minutes passed since disconnect. Allowing reconnections for the next session.");
    }

    // Background Reconnection Loop
    // If we are fully disconnected and not in the cool-down period, we ensure the ESP32 is connectable
    // and periodically try to page the speaker.
    if (btAudio.isDisconnected() && !reconnectPending) {
        static uint32_t lastReconnectTry = 0;
        if (millis() - lastReconnectTry > 15000) {
            lastReconnectTry = millis();
            
            // 1. Force the ESP32 to be connectable and discoverable
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            
            // 2. Actively try to page the speaker
            btAudio.reconnect();
        }
    }
}
