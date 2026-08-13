#pragma once

#include <Arduino.h>
#include <freertos/ringbuf.h>
#include <vector>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

enum TimerState {
    TIMER_30_MIN = 0,
    TIMER_60_MIN,
    TIMER_ENDLESS
};

struct ScannedDevice {
    String name;
    String bda_str;
    esp_bd_addr_t bda;
    uint32_t lastSeen;
};

class BTAudio {
public:
    BTAudio();
    
    static String pendingDeviceName;
    void begin(const std::vector<String>& savedDevices);
    
    void connectTo(const String& name);
    void disconnect();
    
    void loop();
    
    bool isPaused = false;

    void playAnnouncement(const char* filepath);
    void handleButton();
    void resetTimer();
    void reconnect();
    
    void nextNoiseTrack();
    void toggleTimer();
    
    volatile bool toggleTimerPending = false;
    volatile bool nextTrackPending = false;
    
    bool isConnected();
    bool isDisconnected();
    bool isAnnouncementPlaying();
    
    // Scanning methods
    void startScan();
    std::vector<String> getScanResults();
    
    TimerState getTimerState() { return timerState; }
    uint32_t getTimerStartTime() { return timerStartTime; }
    void setTimerState(TimerState state) { timerState = state; }

    uint32_t lastPauseTime;
    
    volatile bool mediaReadyPending = false;
    volatile uint32_t connectedTime = 0;

private:
    TimerState timerState;
    uint32_t timerStartTime;
    
    void initBluetooth();
};

extern BTAudio btAudio;
