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
    
    // Save and load last selected noise type index
    void saveLastNoiseType(int typeIndex);
    int getLastNoiseType();
    

private:
    // We use the Preferences library internally
};

extern Storage storage;
