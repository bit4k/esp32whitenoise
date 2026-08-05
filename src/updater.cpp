#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

const char* version_url = "https://ota.62.herleth.de/esp32whitenoise/version.json";
const char* firmware_url = "https://ota.62.herleth.de/esp32whitenoise/firmware.bin";

void setup() {
    Serial.begin(115200);
    Serial.println("\n--- UPDATER FIRMWARE ---");

    WiFi.setHostname("white-noise");
    WiFi.begin(); // Use credentials saved in NVS by the main app
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFailed to connect to WiFi! Rebooting to main app...");
        esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
        esp_restart();
    }
    
    Serial.println("\nWiFi connected. Downloading firmware...");

    HTTPClient http;
    http.begin(firmware_url);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        int contentLength = http.getSize();
        bool canBegin = Update.begin(contentLength, U_FLASH);
        if (canBegin) {
            Serial.println("Begin OTA. Please wait...");
            size_t written = Update.writeStream(http.getStream());
            if (written == contentLength) {
                Serial.println("Written : " + String(written) + " successfully");
            } else {
                Serial.println("Written only : " + String(written) + "/" + String(contentLength));
            }
            if (Update.end()) {
                Serial.println("OTA done!");
            } else {
                Serial.println("Error Occurred. Error #: " + String(Update.getError()));
            }
        } else {
            Serial.println("Not enough space to begin OTA");
        }
    } else {
        Serial.println("Failed to download firmware. HTTP Code: " + String(httpCode));
    }
    http.end();
    
    Serial.println("Rebooting to main app...");
    // Update.end() already sets boot partition on success, but this ensures it 
    // also falls back to the main app if the update failed or was skipped.
    esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    esp_restart();
}

void loop() {
}
