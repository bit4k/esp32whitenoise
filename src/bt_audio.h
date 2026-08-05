#pragma once

#include <Arduino.h>
#include <freertos/ringbuf.h>
#include <vector>
#include <BluetoothA2DPSource.h>

enum TimerState {
    TIMER_30_MIN = 0,
    TIMER_60_MIN,
    TIMER_ENDLESS
};

struct ScannedDevice {
    String name;
    uint32_t lastSeen;
};

class MyA2DPSource : public BluetoothA2DPSource {
public:
    std::vector<ScannedDevice> foundDevices;
    void filter_inquiry_scan_result(esp_bt_gap_cb_param_t* param) override;
};

class BTAudio {
public:
    BTAudio();
    
    static String pendingDeviceName;
    void begin(const std::vector<String>& savedDevices);
    
    void connectTo(const String& mac);
    void disconnect();
    
    void loop();
    
    void playAnnouncement(const char* filepath);
    
    void nextNoiseTrack();
    void toggleTimer();
    
    bool isConnected();
    bool isAnnouncementPlaying();
    
    // Scanning methods
    void startScan();
    std::vector<String> getScanResults();
    
    TimerState getTimerState() { return timerState; }
    uint32_t getTimerStartTime() { return timerStartTime; }
    void setTimerState(TimerState state) { timerState = state; }

private:
    TimerState timerState;
    uint32_t timerStartTime;
    uint32_t lastPauseTime;
    std::vector<String> _targetDevices;
    
    static int32_t audio_data_callback(uint8_t *data, int32_t len);
    static void avrc_cmd_callback(uint8_t cmd);
    static void connection_state_callback(uint8_t state);
};

extern BTAudio btAudio;
