#pragma once

#include <Arduino.h>
#include <vector>

class Storage {
public:
    Storage();
    void begin();
    
    // Saved BT devices management
    std::vector<String> getSavedDevices();
    void addSavedDevice(const String& name);
    void removeSavedDevice(const String& name);
    
    // Pending device logic for connection verification
    void setPendingDevice(const String& name);
    String popPendingDevice();
    
    // Save and load last MAC address
    void saveSavedMac(const uint8_t* bda);
    bool getSavedMac(uint8_t* bda);
    
    // Save and load last selected noise type index
    void saveLastNoiseType(int typeIndex);
    int getLastNoiseType();
    

private:
    // We use the Preferences library internally
};

extern Storage storage;
