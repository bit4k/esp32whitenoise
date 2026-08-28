#include "web_server.h"
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include "storage.h"
#include "bt_audio.h"
#include <ArduinoJson.h>
#include "AsyncJson.h"

extern bool timerExpired;
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <esp_ota_ops.h>

WebServerManager webServer;
AsyncWebServer server(80);

const char* html_page = R"HTML(<!DOCTYPE html>
<html>
<head>
    <title>ESP32 White Noise</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: sans-serif; background: #121212; color: #ffffff; margin: 0; padding: 20px; text-align: center; }
        h1 { color: #bb86fc; }
        .card { background: #1e1e1e; border-radius: 10px; padding: 20px; max-width: 400px; margin: 0 auto 20px; box-shadow: 0 4px 8px rgba(0,0,0,0.5); }
        input[type="text"] { width: 80%; padding: 10px; margin: 10px 0; border: none; border-radius: 5px; background: #2c2c2c; color: white; }
        button { background: #bb86fc; color: #000; border: none; padding: 10px 20px; border-radius: 5px; font-weight: bold; cursor: pointer; margin: 5px; }
        button:hover { background: #3700b3; color: #fff; }
        .danger { background: #cf6679; color: #000; }
    </style>
</head>
<body>
    <h1>White Noise Generator</h1>
    <div class="card">
        <h2>Bluetooth Pairing</h2>
        <h3>Saved Speakers:</h3>
        <div id="savedDevicesList">Loading...</div>
        <hr style="border:1px solid #444; margin:15px 0;">
        <button onclick="scanBT()">Scan for Speakers</button>
        <div id="scanResults" style="margin:10px 0; max-height:150px; overflow-y:auto; text-align:left; background:#2c2c2c; border-radius:5px; padding:5px;"></div>
        <br>
        <input type="text" id="mac" placeholder="Speaker Name or MAC">
        <br>
        <button onclick="saveMac()">Connect</button>
    </div>
    <div class="card">
        <h2>System</h2>
        <button onclick="ota()">Update Firmware (OTA)</button>
    </div>
    <script>
        function loadStatus() {
            fetch('/api/status').then(function(r){return r.json();}).then(function(d){
                var html = "";
                if (d.devices && d.devices.length > 0) {
                    for (var i = 0; i < d.devices.length; i++) {
                        var name = d.devices[i];
                        html += '<div style="display:flex; justify-content:space-between; background:#2c2c2c; padding:8px; margin-bottom:5px; border-radius:5px; align-items:center;">' +
                            '<span>' + name + '</span>' +
                            '<button class="danger" onclick="deleteMac(\'' + name + '\')">Delete</button>' +
                        '</div>';
                    }
                } else {
                    html = "<p style='color:#aaa;'>No speakers saved yet.</p>";
                }
                if (d.pending_mac && d.pending_mac !== "") {
                    html = '<div style="background:#b08d00; color:#fff; padding:10px; margin-bottom:10px; border-radius:5px;">' +
                        '<b>Connecting to ' + d.pending_mac + '...</b><br>Please wait up to 20 seconds.' +
                    '</div>' + html;
                }
                document.getElementById('savedDevicesList').innerHTML = html;
            });
        }
        loadStatus();
        function saveMac() {
            var m = document.getElementById('mac').value;
            if (m) connectMac(m);
        }
        function connectMac(m) {
            fetch('/api/connect?mac=' + encodeURIComponent(m)).then(function(){
                location.reload();
            });
        }
        function deleteMac(name) {
            if (confirm("Delete " + name + "?")) {
                fetch('/api/delete?mac=' + encodeURIComponent(name)).then(function(){
                    location.reload();
                });
            }
        }
        function ota() {
            fetch('/api/ota').then(function(){ alert('OTA started...'); });
        }
        function scanBT() {
            var resDiv = document.getElementById('scanResults');
            resDiv.innerHTML = "<div style='padding:10px;text-align:center;'>Scanning...</div>";
            fetch('/api/bt/results').then(function(r){return r.json();}).then(function(d){
                var html = "";
                if (!d || d.length === 0) {
                    html = "<div style='padding:10px;text-align:center;color:#aaa;'>No devices found nearby.<br><small>(Ensure speaker is in pairing mode)</small></div>";
                } else {
                    for (var i = 0; i < d.length; i++) {
                        var name = d[i];
                        html += "<div style='padding:5px; border-bottom:1px solid #444;'>" +
                            "<button style='width:100%; text-align:left; background:transparent; color:#bb86fc; font-size:15px;' onclick='connectMac(\"" + name + "\")'>" +
                                "Connect: " + name +
                            "</button>" +
                        "</div>";
                    }
                }
                resDiv.innerHTML = html;
            });
        }
    </script>
</body>
</html>)HTML";

WebServerManager::WebServerManager() {
    otaRequested = false;
    connectRequested = false;
    pendingRestartTime = 0;
}

void WebServerManager::scheduleConnect(const String& name) {
    targetDeviceName = name;
    pendingRestartTime = millis() + 400;
}

void WebServerManager::begin() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        Serial.printf("[WebServer] GET / requested from %s - Free heap: %u\n", 
                      request->client()->remoteIP().toString().c_str(), ESP.getFreeHeap());
        request->send(200, "text/html", html_page);
    });
    
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
        Serial.printf("[WebServer] GET /api/status\n");
        std::vector<String> devs = storage.getSavedDevices();
        AsyncJsonResponse * response = new AsyncJsonResponse();
        JsonVariant& root = response->getRoot();
        JsonArray arr = root["devices"].to<JsonArray>();
        for(auto& d : devs) {
            arr.add(d);
        }
        root["timer"] = btAudio.getTimerState();
        root["timerExpired"] = timerExpired;
        root["pending_mac"] = BTAudio::pendingDeviceName;
        response->setLength();
        request->send(response);
    });
    
    server.on("/api/connect", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("mac")) {
            String mac = request->getParam("mac")->value();
            Serial.printf("[WebServer] GET /api/connect?mac=%s\n", mac.c_str());
            webServer.scheduleConnect(mac); 
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing MAC/Name");
        }
    });
    
    server.on("/api/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("mac")) {
            String mac = request->getParam("mac")->value();
            storage.removeSavedDevice(mac);
            request->send(200, "text/plain", "OK");
        } else {
            request->send(400, "text/plain", "Missing MAC/Name");
        }
    });

    server.on("/api/bt/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        btAudio.startScan();
        request->send(200, "text/plain", "OK");
    });

    server.on("/api/bt/results", HTTP_GET, [](AsyncWebServerRequest *request){
        std::vector<String> results = btAudio.getScanResults();
        AsyncJsonResponse * response = new AsyncJsonResponse(true); // true = array
        JsonVariant& root = response->getRoot();
        for (auto& r : results) {
            root.add(r);
        }
        response->setLength();
        request->send(response);
    });
    
    server.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Starting OTA...");
        webServer.triggerOTA();
    });

    server.onNotFound([](AsyncWebServerRequest *request){
        Serial.printf("[WebServer] HTTP %s %s requested from %s\n", 
                      request->methodToString(), request->url().c_str(), request->client()->remoteIP().toString().c_str());
        request->send(404, "text/plain", "Not Found");
    });

    server.begin();
}

void WebServerManager::triggerOTA() {
    otaRequested = true;
}

void WebServerManager::autoCheckOTA() {
    // Current version defined in firmware
    const char* CURRENT_VERSION = "1.0.0";
    
    WiFiClient client; // KEIN Secure (spart >40KB RAM!)
    HTTPClient http;
    
    const char* url = "http://ota.62.herleth.de/esp32whitenoise/version.json";
    if (http.begin(client, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);
            
            if (!error) {
                const char* remote_version = doc["version"];
                const char* firmware_file = doc["firmware"];
                
                if (remote_version && firmware_file) {
                    if (String(remote_version) != String(CURRENT_VERSION)) {
                        Serial.println("New version available! Triggering OTA...");
                        triggerOTA();
                    } else {
                        Serial.println("Firmware is up to date.");
                    }
                }
            }
        }
        http.end();
    }
}

void WebServerManager::loop() {
    if (pendingRestartTime > 0 && millis() >= pendingRestartTime) {
        pendingRestartTime = 0;
        Serial.printf("[WebServer] Saving target device '%s' permanently and rebooting...\n", targetDeviceName.c_str());
        storage.addSavedDevice(targetDeviceName);
        storage.setPendingDevice(targetDeviceName);
        delay(100);
        ESP.restart();
    }

    if (otaRequested) {
        otaRequested = false;
        Serial.println("OTA Update requested. Rebooting into factory partition...");
        
        // Find the factory partition
        const esp_partition_t* factory = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (factory) {
            esp_ota_set_boot_partition(factory);
            esp_restart();
        } else {
            Serial.println("Factory partition not found!");
        }
    }
}
