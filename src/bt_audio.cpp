#include "bt_audio.h"
#include "noise_gen.h"
#include "storage.h"
#include <BluetoothA2DPSource.h>
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

BTAudio btAudio;
MyA2DPSource a2dp_source;

// Implementation of our custom BT scanner
void MyA2DPSource::filter_inquiry_scan_result(esp_bt_gap_cb_param_t* param) {
    esp_bt_gap_dev_prop_t *p;
    uint8_t *eir = nullptr;
    uint8_t *bdname_ptr = nullptr;
    
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        p = param->disc_res.prop + i;
        if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
            eir = (uint8_t *)(p->val);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            bdname_ptr = (uint8_t *)(p->val);
        }
    }
    
    char name_str[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = {0};
    if (bdname_ptr) {
        strncpy(name_str, (char*)bdname_ptr, ESP_BT_GAP_MAX_BDNAME_LEN);
    } else if (eir) {
        uint8_t len = 0;
        if (get_name_from_eir(eir, (uint8_t*)name_str, &len)) {
            name_str[len] = '\0';
        }
    }
    
    if (strlen(name_str) > 0) {
        String name = String(name_str);
        bool updated = false;
        for (auto& dev : foundDevices) {
            if (dev.name == name) {
                dev.lastSeen = millis();
                updated = true;
                break;
            }
        }
        if (!updated) {
            foundDevices.push_back({name, millis()});
            Serial.printf("Found BT Device: %s\n", name.c_str());
        }
    }
    // Call the original logic to allow connections to continue if matching
    BluetoothA2DPSource::filter_inquiry_scan_result(param);
}

// ESP8266Audio objects
AudioFileSourceSPIFFS *fileSource = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;

// Custom AudioOutput that writes to a FreeRTOS RingBuffer
class AudioOutputRingBuf : public AudioOutput {
public:
    RingbufHandle_t rb;
    AudioOutputRingBuf() {
        rb = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF); // 4KB buffer to free up heap for BT stack during connection
    }
    ~AudioOutputRingBuf() {
        if (rb) vRingbufferDelete(rb);
    }
    virtual bool begin() override { return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!rb) return false;
        // Wait up to 10ms for space
        xRingbufferSend(rb, sample, 4, pdMS_TO_TICKS(10));
        return true;
    }
    virtual bool stop() override { return true; }
};

AudioOutputRingBuf *outBuf = nullptr;

BTAudio::BTAudio() {
    timerState = TIMER_30_MIN;
    timerStartTime = 0;
    lastPauseTime = 0;
}

void BTAudio::begin(const std::vector<String>& savedDevices) {
    outBuf = new AudioOutputRingBuf();
    
    // Copy to our member variable so the memory stays valid
    _targetDevices = savedDevices;
    
    // Set callbacks
    a2dp_source.set_auto_reconnect(false);
    a2dp_source.set_reset_ble(false); // Prevents esp_bt_controller_mem_release(BLE) crash on ESP-IDF 5
    
    
    if (_targetDevices.size() == 0) {
        a2dp_source.start_raw(BTAudio::audio_data_callback);
    } else if (_targetDevices.size() == 1) {
        a2dp_source.start_raw(_targetDevices[0].c_str(), BTAudio::audio_data_callback);
    } else if (_targetDevices.size() > 1) {
        std::vector<const char*> names;
        for (const String& d : _targetDevices) {
            names.push_back(d.c_str());
        }
        a2dp_source.start_raw(names, BTAudio::audio_data_callback);
    }
    
    // NOTE: ESP32-A2DP source AVRCP support for receiving commands from sink 
    // requires setting up the avrc callback.
    a2dp_source.set_avrc_passthru_command_callback([](uint8_t cmd, bool key_state) {
        if (!key_state) { // false/0 = pressed
            if (cmd == ESP_AVRC_PT_CMD_PAUSE || cmd == ESP_AVRC_PT_CMD_STOP) {
                BTAudio::avrc_cmd_callback(0); // Pause
            } else if (cmd == ESP_AVRC_PT_CMD_PLAY) {
                BTAudio::avrc_cmd_callback(1); // Play
            }
        }
    });

    timerStartTime = millis();
}

void BTAudio::connectTo(const String& mac) {
    if (isConnected()) {
        a2dp_source.disconnect();
    }
    storage.addSavedDevice(mac);
    // Reboot must be handled by the caller after sending HTTP response
}

void BTAudio::disconnect() {
    if (isConnected()) {
        a2dp_source.disconnect();
    }
}

bool BTAudio::isConnected() {
    return a2dp_source.is_connected();
}

void BTAudio::startScan() {
    // No need to clear or start manually since the library loops discovery in the background when disconnected
    // We just keep the devices in the list until they time out
}

std::vector<String> BTAudio::getScanResults() {
    std::vector<String> activeDevices;
    uint32_t now = millis();
    
    for (auto it = a2dp_source.foundDevices.begin(); it != a2dp_source.foundDevices.end(); ) {
        if (now - it->lastSeen > 60000) {
            // Remove devices not seen in the last 60 seconds
            it = a2dp_source.foundDevices.erase(it);
        } else {
            activeDevices.push_back(it->name);
            ++it;
        }
    }
    return activeDevices;
}

void BTAudio::playAnnouncement(const char* filepath) {
    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
    if (fileSource) delete fileSource;
    if (mp3) delete mp3;
    
    fileSource = new AudioFileSourceSPIFFS(filepath);
    mp3 = new AudioGeneratorMP3();
    mp3->begin(fileSource, outBuf);
}

bool BTAudio::isAnnouncementPlaying() {
    return (mp3 && mp3->isRunning());
}

void BTAudio::loop() {
    if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) {
            mp3->stop();
            delete mp3; mp3 = nullptr;
            delete fileSource; fileSource = nullptr;
        }
    }
}

void BTAudio::nextNoiseTrack() {
    int t = noiseGen.getType();
    t = (t + 1) % NUM_NOISE_TYPES;
    noiseGen.setType(t);
    storage.saveLastNoiseType(t);
    
    // Play announcement for new noise track
    String filename = String("/") + String(t) + ".mp3";
    playAnnouncement(filename.c_str());
}

void BTAudio::toggleTimer() {
    if (timerState == TIMER_30_MIN) {
        timerState = TIMER_60_MIN;
        playAnnouncement("/timer_60.mp3");
    } else if (timerState == TIMER_60_MIN) {
        timerState = TIMER_ENDLESS;
        playAnnouncement("/timer_endless.mp3");
    } else {
        timerState = TIMER_30_MIN;
        playAnnouncement("/timer_30.mp3");
    }
    timerStartTime = millis();
}

void BTAudio::avrc_cmd_callback(uint8_t cmd) {
    // 0 = Pause, 1 = Play
    if (cmd == 0) {
        btAudio.lastPauseTime = millis();
    } else if (cmd == 1) {
        if (millis() - btAudio.lastPauseTime < 1000) {
            // Play pressed within 1 second of pause -> toggle timer
            btAudio.toggleTimer();
        }
    }
}

extern bool timerExpired;

int32_t BTAudio::audio_data_callback(uint8_t *data, int32_t len) {
    if (!data || len <= 0) return 0;
    
    // If Timer expired, send silence
    if (timerExpired) {
        memset(data, 0, len);
        return len;
    }
    
    // If MP3 is playing and we have data in the ring buffer, use it
    if (outBuf && outBuf->rb) {
        size_t bytes_received;
        uint8_t *rb_data = (uint8_t *)xRingbufferReceiveUpTo(outBuf->rb, &bytes_received, 0, len);
        
        if (rb_data && bytes_received > 0) {
            memcpy(data, rb_data, bytes_received);
            vRingbufferReturnItem(outBuf->rb, (void *)rb_data);
            
            // If we didn't get enough bytes to fill 'len', fill the rest with silence
            if (bytes_received < len) {
                memset(data + bytes_received, 0, len - bytes_received);
            }
            return len;
        }
    }
    
    // Otherwise, generate noise
    int32_t frameCount = len / 4; // 1 frame = 4 bytes (2 channels * 16-bit)
    noiseGen.getFrames((NoiseGenerator::Frame*)data, frameCount);
    return len;
}
