#include "storage.h"
#include <Preferences.h>
#include <ArduinoJson.h>

Storage storage;
Preferences prefs;

Storage::Storage() {
}

void Storage::begin() {
    prefs.begin("wnoise", false); // false = read/write
}

std::vector<String> Storage::getSavedDevices() {
    std::vector<String> devices;
    String json = prefs.getString("devices", "[]");
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);
    if (!error) {
        for (JsonVariant v : doc.as<JsonArray>()) {
            devices.push_back(v.as<String>());
        }
    }
    
    // Backwards compatibility migration
    String old_mac = "";
    if (prefs.isKey("last_mac")) {
        old_mac = prefs.getString("last_mac", "");
    }
    if (old_mac.length() > 0) {
        prefs.remove("last_mac"); // Remove FIRST to prevent recursion
        if (std::find(devices.begin(), devices.end(), old_mac) == devices.end()) {
            devices.push_back(old_mac);
            
            JsonDocument doc;
            for (const String& d : devices) {
                doc.add(d);
            }
            String new_json;
            serializeJson(doc, new_json);
            prefs.putString("devices", new_json);
        }
    }
    return devices;
}

void Storage::addSavedDevice(const String& name) {
    std::vector<String> devs = getSavedDevices();
    if (std::find(devs.begin(), devs.end(), name) == devs.end()) {
        devs.push_back(name);
        JsonDocument doc;
        for (const String& d : devs) {
            doc.add(d);
        }
        String json;
        serializeJson(doc, json);
        prefs.putString("devices", json);
    }
}

void Storage::removeSavedDevice(const String& name) {
    std::vector<String> devs = getSavedDevices();
    auto it = std::find(devs.begin(), devs.end(), name);
    if (it != devs.end()) {
        devs.erase(it);
        JsonDocument doc;
        for (const String& d : devs) {
            doc.add(d);
        }
        String json;
        serializeJson(doc, json);
        prefs.putString("devices", json);
    }
}

void Storage::setPendingDevice(const String& name) {
    prefs.putString("pending_mac", name);
}

String Storage::popPendingDevice() {
    String pending = "";
    if (prefs.isKey("pending_mac")) {
        pending = prefs.getString("pending_mac", "");
    }
    if (pending.length() > 0) {
        prefs.remove("pending_mac");
    }
    return pending;
}

void Storage::saveLastNoiseType(int typeIndex) {
    prefs.putInt("noise_type", typeIndex);
}

int Storage::getLastNoiseType() {
    return prefs.getInt("noise_type", 0);
}


