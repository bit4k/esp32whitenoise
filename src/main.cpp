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
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <nvs_flash.h>
#include <nvs.h>

WiFiManager wm;
const int LED_PIN = 5;       // Wemos LOLIN32 / ESP32 LED Pin
const int BOOT_BTN_PIN = 0;  // Non-reset button (ESP32 BOOT button / GPIO 0)

static bool configModeActive = false;

void handleLED() {
    static uint32_t lastCycle = 0;
    static bool ledActive = false;
    uint32_t now = millis();

    if (configModeActive) {
        // Config Mode (Web Server active): Fast 150ms blink so user knows setup portal is running
        if (now - lastCycle >= 150) {
            lastCycle = now;
            ledActive = !ledActive;
            digitalWrite(LED_PIN, ledActive ? LOW : HIGH);
        }
        return;
    }

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
        // Disconnected / Normal: LED ALWAYS OFF (Active LOW -> HIGH is OFF)
        ledActive = false;
        digitalWrite(LED_PIN, HIGH);
    }
}

// Function to start WiFi and Web Server on demand
void startConfigServer() {
    if (configModeActive) return;
    configModeActive = true;
    
    Serial.println("\n============================================");
    Serial.println("[ConfigMode] Starting WiFi & Web Configuration Portal...");
    Serial.println("============================================\n");
    
    WiFi.setHostname("white-noise");
    
    wm.setAPCallback([](WiFiManager *myWiFiManager) {
        Serial.println("\n============================================");
        Serial.println("[WiFiManager] SoftAP Config Portal Started!");
        Serial.printf("[WiFiManager] AP SSID: %s\n", myWiFiManager->getConfigPortalSSID().c_str());
        Serial.printf("[WiFiManager] AP IP Address: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.println("============================================\n");
    });
    
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(10);
    
    bool res = wm.autoConnect("ESP32_WhiteNoise_Setup");
    if (res) {
        Serial.println("\n============================================");
        Serial.println("[WiFi] Connected to WiFi!");
        Serial.printf("[WiFi] STA IP Address: %s\n", WiFi.localIP().toString().c_str());
        Serial.println("============================================\n");
        
        wm.stopWebPortal();
        wm.stopConfigPortal();
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_5dBm);
        WiFi.setSleep(WIFI_PS_MAX_MODEM);
        
        webServer.begin();
        
        if (MDNS.begin("white-noise")) {
            Serial.println("[mDNS] Responder started: http://white-noise.local");
        }
        Serial.printf("[ConfigMode] Web interface ready: http://%s or http://white-noise.local\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[ConfigMode] WiFi connection timed out or closed.");
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
    
    // Init BOOT Button (GPIO 0, active LOW with internal pullup)
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    
    // Init Storage
    storage.begin();
    
    // Clean any corrupted Bluedroid NVS cache from previous abrupt aborts to prevent boot loops
    nvs_handle_t bt_cfg;
    if (nvs_open("bt_config.conf", NVS_READWRITE, &bt_cfg) == ESP_OK) {
        nvs_erase_all(bt_cfg);
        nvs_commit(bt_cfg);
        nvs_close(bt_cfg);
        Serial.println("[BTAudio] Cleared Bluedroid NVS cache to prevent corruption boot loops.");
    }
    
    // Set ESP-IDF Log Levels (Bluetooth on DEBUG)
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("BT_AV", ESP_LOG_DEBUG);
    esp_log_level_set("BT_APP", ESP_LOG_DEBUG);
    esp_log_level_set("BT_BTC", ESP_LOG_DEBUG);
    esp_log_level_set("BT_BTM", ESP_LOG_DEBUG);
    esp_log_level_set("BT_GAP", ESP_LOG_DEBUG);
    esp_log_level_set("BT_A2D", ESP_LOG_DEBUG);
    esp_log_level_set("BT_AVRC", ESP_LOG_DEBUG);
    esp_log_level_set("BTAudio", ESP_LOG_DEBUG);
    esp_log_level_set("NoiseGen", ESP_LOG_DEBUG);

    std::vector<String> devs = storage.getSavedDevices();
    
    // Init SPIFFS
    if(!SPIFFS.begin(true)){
        Serial.println("SPIFFS Mount Failed");
        return;
    }
    
    // Load last noise type
    int type = storage.getLastNoiseType();
    noiseGen.setType(type);
    
    // Check if BOOT button is held on startup OR if no speaker is configured yet
    bool btnHeld = (digitalRead(BOOT_BTN_PIN) == LOW);
    bool hasSavedSpeaker = (!devs.empty() && devs[0].length() > 0);
    
    if (btnHeld || !hasSavedSpeaker) {
        if (btnHeld) {
            Serial.println("[Boot] BOOT button held -> Entering Web Configuration Mode!");
        } else {
            Serial.println("[Boot] No speaker configured yet -> Entering Web Configuration Mode!");
        }
        startConfigServer();
    } else {
        Serial.println("[Boot] Fast Audio Mode: WiFi OFF. Zero radio interference, instant Bluetooth audio!");
        WiFi.mode(WIFI_OFF);
    }
    
    // Initialize Bluetooth Audio IMMEDIATELY!
    btAudio.begin(devs);
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
}

void loop() {
    // Check if user presses the BOOT button for >1s during runtime to open the config portal
    static uint32_t btnPressStart = 0;
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        if (btnPressStart == 0) {
            btnPressStart = millis();
        } else if (millis() - btnPressStart > 1000 && !configModeActive) {
            Serial.println("\n[Button] BOOT button pressed for 1s -> Launching Web Configuration Portal!");
            startConfigServer();
        }
    } else {
        btnPressStart = 0;
    }

    // Process async tasks
    if (configModeActive) {
        webServer.loop();
    }
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
    // If we are fully disconnected and not in the cool-down period, periodically attempt reconnection.
    if (btAudio.isDisconnected() && !reconnectPending) {
        static uint32_t lastReconnectTry = 0;
        if (millis() - lastReconnectTry > 1000) {
            lastReconnectTry = millis();
            btAudio.reconnect();
        }
    }
}
