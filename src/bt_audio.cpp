#include "bt_audio.h"
#include "noise_gen.h"
#include "storage.h"
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include "esp_log.h"

#define BT_AV_TAG "BTAudio"

BTAudio btAudio;

// -------------------------------------------------------------
// Global variables for ESP-IDF Bluetooth State
// -------------------------------------------------------------
static esp_a2d_connection_state_t s_a2d_conn_state = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
static esp_a2d_audio_state_t s_a2d_audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
static esp_bd_addr_t s_peer_bda;
static bool s_has_peer_bda = false;
static bool s_is_discovering = false;
static bool s_pending_connect = false;

// Audio processing
extern bool timerExpired;
class AudioOutputRingBuf : public AudioOutput {
public:
    RingbufHandle_t rb;
    AudioOutputRingBuf() {
        SetGain(2.5f);
        rb = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
    }
    ~AudioOutputRingBuf() {
        if (rb) vRingbufferDelete(rb);
    }
    virtual bool begin() override { return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!rb) return false;
        MakeSampleStereo16(sample);
        int16_t s[2];
        s[0] = Amplify(sample[0]);
        s[1] = Amplify(sample[1]);
        xRingbufferSend(rb, s, 4, pdMS_TO_TICKS(10));
        return true;
    }
    virtual bool stop() override { return true; }
};

static AudioFileSourceSPIFFS *fileSource = nullptr;
static AudioGeneratorMP3 *mp3 = nullptr;
static AudioOutputRingBuf *outBuf = nullptr;
static std::vector<ScannedDevice> s_foundDevices;
static std::vector<String> s_targetDevices;
static uint8_t s_mp3DecoderSpace[24000]; // Static BSS allocation to prevent heap OOM

String BTAudio::pendingDeviceName = "";

// Beep Generator State
static volatile uint32_t s_beepSamplesRemaining = 0;
static float s_beepPhase = 0.0f;
static const float S_BEEP_FREQ = 440.0f; // 440 Hz
static const float S_BEEP_SAMPLE_RATE = 44100.0f;
static const float S_BEEP_TWO_PI = 6.283185307179586f;
static const uint32_t S_BEEP_TOTAL_FRAMES = 17640; // 400ms at 44.1kHz
static const uint32_t S_BEEP_ATTACK_FRAMES = 2205;  // 50ms
static const uint32_t S_BEEP_DECAY_FRAMES = 2205;   // 50ms

void BTAudio::triggerWarningBeep() {
    s_beepPhase = 0.0f;
    s_beepSamplesRemaining = S_BEEP_TOTAL_FRAMES;
}

// -------------------------------------------------------------
// ESP-IDF Callbacks
// -------------------------------------------------------------
static int32_t audio_data_callback(uint8_t *data, int32_t len) {
    if (!data || len <= 0) return 0;
    
    if (btAudio.isPaused || timerExpired) {
        memset(data, 0, len);
        return len;
    }
    
    bool playingAnnouncement = btAudio.isAnnouncementPlaying();
    
    if (outBuf && outBuf->rb) {
        size_t bytes_received = 0;
        uint8_t *rb_data = (uint8_t *)xRingbufferReceiveUpTo(outBuf->rb, &bytes_received, 0, len);
        if (rb_data && bytes_received > 0) {
            memcpy(data, rb_data, bytes_received);
            vRingbufferReturnItem(outBuf->rb, rb_data);
            if (bytes_received < len) {
                int32_t remaining = len - bytes_received;
                if (playingAnnouncement) {
                    memset(data + bytes_received, 0, remaining);
                } else {
                    noiseGen.getFrames((NoiseGenerator::Frame*)(data + bytes_received), remaining / 4);
                }
            }
            return len;
        }
    }
    
    if (playingAnnouncement) {
        memset(data, 0, len);
    } else {
        noiseGen.getFrames((NoiseGenerator::Frame*)data, len / 4);
    }
    
    // Apply Volume Fade & Overlay Warning Beep if playing normal audio/noise
    if (!playingAnnouncement) {
        float currentFade = btAudio.getFadeFactor();
        int16_t *samples = (int16_t *)data;
        int32_t num_frames = len / 4; // 4 bytes per stereo frame
        
        for (int32_t i = 0; i < num_frames; i++) {
            int16_t left = samples[i * 2];
            int16_t right = samples[i * 2 + 1];
            
            // 1. Apply volume fade factor
            if (currentFade < 1.0f) {
                left = (int16_t)(left * currentFade);
                right = (int16_t)(right * currentFade);
            }
            
            // 2. Overlay warning beep if active
            if (s_beepSamplesRemaining > 0) {
                uint32_t framesPlayed = S_BEEP_TOTAL_FRAMES - s_beepSamplesRemaining;
                float env = 0.12f; // max volume = 12%
                
                if (framesPlayed < S_BEEP_ATTACK_FRAMES) {
                    env *= ((float)framesPlayed / (float)S_BEEP_ATTACK_FRAMES);
                } else if (s_beepSamplesRemaining < S_BEEP_DECAY_FRAMES) {
                    env *= ((float)s_beepSamplesRemaining / (float)S_BEEP_DECAY_FRAMES);
                }
                
                float toneSample = sinf(s_beepPhase) * env * 32767.0f;
                s_beepPhase += (S_BEEP_TWO_PI * S_BEEP_FREQ / S_BEEP_SAMPLE_RATE);
                if (s_beepPhase >= S_BEEP_TWO_PI) {
                    s_beepPhase -= S_BEEP_TWO_PI;
                }
                
                int32_t mixedL = (int32_t)left + (int32_t)toneSample;
                int32_t mixedR = (int32_t)right + (int32_t)toneSample;
                
                left = (mixedL > 32767) ? 32767 : ((mixedL < -32768) ? -32768 : (int16_t)mixedL);
                right = (mixedR > 32767) ? 32767 : ((mixedR < -32768) ? -32768 : (int16_t)mixedR);
                
                s_beepSamplesRemaining--;
            }
            
            samples[i * 2] = left;
            samples[i * 2 + 1] = right;
        }
    }
    return len;
}

static void bt_app_av_sm_hdlr(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        s_a2d_conn_state = param->conn_stat.state;
        if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, "A2DP Connected");
            memcpy(s_peer_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            s_has_peer_bda = true;
            btAudio.isPaused = false; // Reset pause on connect
            btAudio.mediaReadyPending = true;
            btAudio.connectedTime = millis();
            
            if (BTAudio::pendingDeviceName != "") {
                storage.addSavedDevice(BTAudio::pendingDeviceName);
                Serial.printf("[BTAudio] Successfully connected to %s. Saved!\n", BTAudio::pendingDeviceName.c_str());
                BTAudio::pendingDeviceName = "";
            }
        } else if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(BT_AV_TAG, "A2DP Disconnected");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        s_a2d_audio_state = param->audio_stat.state;
        if (s_a2d_audio_state == ESP_A2D_AUDIO_STATE_STARTED) {
            Serial.println("[BTAudio] A2DP Audio Started");
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            Serial.println("[BTAudio] Media ready, starting...");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        }
        break;
    }
    default:
        break;
    }
}

static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
        uint8_t *bda = param->conn_stat.remote_bda;
        Serial.printf("[BTAudio] AVRC TG conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]\n",
                 param->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
        if (param->reg_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
            ESP_LOGI(BT_AV_TAG, "AVRCP RN_PLAY_STATUS_CHANGE registered");
            esp_avrc_rn_param_t rn_param;
            rn_param.playback = ESP_AVRC_PLAYBACK_PLAYING;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        } else if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            ESP_LOGI(BT_AV_TAG, "AVRCP RN_VOLUME_CHANGE registered");
            esp_avrc_rn_param_t rn_param;
            rn_param.volume = 127;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
        break;
    }
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        Serial.printf("[BTAudio] AVRCP TG Set Absolute Volume: %d\n", param->set_abs_vol.volume);
        btAudio.resetTimerPending = true;
        break;
    }
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
        if (param->psth_cmd.key_state == 0) { // Pressed
            Serial.printf("[BTAudio] AVRCP PT_CMD %d\n", param->psth_cmd.key_code);
            if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_PLAY) {
                btAudio.isPaused = false;
                // Removed esp_a2d_media_ctrl(START) to prevent rapid-click state corruption crash
                Serial.println("[BTAudio] -> Action: Play");
                // Software double-click fallback
                if (millis() - btAudio.lastPauseTime < 1000) {
                    Serial.println("[BTAudio] -> Software Double-Click Detected!");
                    btAudio.toggleTimerPending = true;
                }
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_PAUSE) {
                btAudio.isPaused = true;
                // Removed esp_a2d_media_ctrl(SUSPEND) to prevent rapid-click state corruption crash
                btAudio.lastPauseTime = millis();
                Serial.println("[BTAudio] -> Action: Pause");
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_FORWARD) {
                Serial.println("[BTAudio] -> Action: Forward (Hardware Double-Click!)");
                btAudio.toggleTimerPending = true;
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_BACKWARD) {
                Serial.println("[BTAudio] -> Action: Backward (Hardware Triple-Click!)");
                btAudio.nextTrackPending = true;
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_UP || param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_DOWN) {
                Serial.println("[BTAudio] -> Volume Button Pressed on Speaker!");
                btAudio.resetTimerPending = true;
            }
        }
        break;
    }
    default:
        break;
    }
}

static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        Serial.printf("[BTAudio] AVRC CT conn_state evt: state %d\n", param->conn_stat.connected);
    } else if (event == ESP_AVRC_CT_CHANGE_NOTIFY_EVT) {
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            Serial.println("[BTAudio] AVRCP CT Volume Change Notification received!");
            btAudio.resetTimerPending = true;
        }
    }
}

static bool get_name_from_eir(uint8_t *eir, char *bdname, uint8_t *len) {
    uint8_t *rmt_bdname = NULL;
    uint8_t rmt_bdname_len = 0;

    if (!eir) {
        return false;
    }

    rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (rmt_bdname) {
        if (rmt_bdname_len > ESP_BT_GAP_MAX_BDNAME_LEN) {
            rmt_bdname_len = ESP_BT_GAP_MAX_BDNAME_LEN;
        }

        if (bdname) {
            memcpy(bdname, rmt_bdname, rmt_bdname_len);
            bdname[rmt_bdname_len] = '\0';
        }
        if (len) {
            *len = rmt_bdname_len;
        }
        return true;
    }
    return false;
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
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
            if (get_name_from_eir(eir, name_str, &len)) {
                name_str[len] = '\0';
            }
        }
        
        if (strlen(name_str) > 0) {
            String name = String(name_str);
            bool updated = false;
            for (auto& dev : s_foundDevices) {
                if (dev.name == name) {
                    dev.lastSeen = millis();
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                ScannedDevice dev;
                dev.name = name;
                dev.lastSeen = millis();
                memcpy(dev.bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                s_foundDevices.push_back(dev);
                Serial.printf("Found BT Device: %s\n", name.c_str());
            }
            
            // Check if it's our target device (case-insensitive or substring match)
            for (const String& target : s_targetDevices) {
                if (target.equalsIgnoreCase(name) || 
                    (name.length() > 0 && target.indexOf(name) >= 0) || 
                    (name.length() > 0 && name.indexOf(target) >= 0)) {
                    Serial.printf("[BTAudio] MATCHED target device '%s'! Stopping discovery to connect...\n", name.c_str());
                    memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    s_has_peer_bda = true;
                    s_pending_connect = true;
                    esp_bt_gap_cancel_discovery();
                    break;
                }
            }
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            Serial.println("[BTAudio] Discovery stopped.");
            s_is_discovering = false;
            if (s_pending_connect && s_has_peer_bda && s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                s_pending_connect = false;
                Serial.println("[BTAudio] Initiating A2DP connection to target device...");
                esp_a2d_source_connect(s_peer_bda);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            Serial.println("[BTAudio] Discovery started.");
            s_is_discovering = true;
        }
        break;
    }
    default:
        break;
    }
}

// -------------------------------------------------------------
// BTAudio Class
// -------------------------------------------------------------
BTAudio::BTAudio() {
    timerState = TIMER_ENDLESS;
    timerStartTime = 0;
    lastPauseTime = 0;
}

void BTAudio::begin(const std::vector<String>& savedDevices) {
    outBuf = new AudioOutputRingBuf();
    
    String pending = storage.popPendingDevice();
    if (pending != "") {
        BTAudio::pendingDeviceName = pending;
        s_targetDevices = {pending};
    } else {
        s_targetDevices = savedDevices;
    }

    Serial.println("[BTAudio] Initialized target devices list:");
    if (s_targetDevices.empty()) {
        Serial.println("  (NONE SAVED YET! Open http://white-noise.local to select your Bluetooth speaker)");
    } else {
        for (const String& d : s_targetDevices) {
            Serial.printf("  - '%s'\n", d.c_str());
        }
    }

    initBluetooth();

    resetTimer();
}

void BTAudio::initBluetooth() {
    if(!btStart()) {
        ESP_LOGE(BT_AV_TAG, "btStart failed");
        return;
    }

    if (esp_bluedroid_init() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "esp_bluedroid_init failed");
        return;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(BT_AV_TAG, "esp_bluedroid_enable failed");
        return;
    }

    // GAP Setup
    esp_bt_gap_register_callback(bt_app_gap_cb);

    // Set default parameters for Secure Simple Pairing (SSP) to allow "Just Works" encryption.
    // Strict devices like Sony speakers often require an encrypted link to send AVRCP commands.
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    // AVRCP Setup
    esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
    esp_avrc_tg_init();
    
    // Set up Passthrough Command Filter for Play, Pause, Forward, Backward
    esp_avrc_psth_bit_mask_t cmd_set = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);

    // Some speakers (like Sony) require the source to also support AVRCP Controller 
    // to properly negotiate Play/Pause command passing!
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(bt_app_rc_ct_cb);
    
    // A2DP Setup
    esp_a2d_register_callback(bt_app_av_sm_hdlr);
    esp_a2d_source_register_data_callback(audio_data_callback);
    esp_a2d_source_init();
    
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

void BTAudio::startScan() {
    static uint32_t lastScanStart = 0;
    static int scanCount = 0;
    
    // Cooldown: after 3 scan attempts without finding target, wait 60s to prevent BT queue overflow
    uint32_t cooldown = (scanCount >= 3) ? 60000 : 15000;
    
    if (!s_is_discovering && (millis() - lastScanStart > cooldown)) {
        lastScanStart = millis();
        if (scanCount >= 3) {
            scanCount = 0;
        }
        scanCount++;
        Serial.printf("[BTAudio] Starting Bluetooth Discovery (Attempt %d/3)...\n", scanCount);
        if (s_targetDevices.empty()) {
            Serial.println("[BTAudio] Warning: No target speaker saved! Open http://white-noise.local in browser to pick your speaker.");
        }
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    }
}

std::vector<String> BTAudio::getScanResults() {
    std::vector<String> res;
    uint32_t now = millis();
    for (auto it = s_foundDevices.begin(); it != s_foundDevices.end(); ) {
        if (now - it->lastSeen > 30000) {
            it = s_foundDevices.erase(it);
        } else {
            res.push_back(it->name);
            ++it;
        }
    }
    return res;
}

void BTAudio::connectTo(const String& name) {
    storage.setPendingDevice(name);
    delay(500);
    ESP.restart();
}

void BTAudio::disconnect() {
    if (isConnected()) {
        esp_a2d_source_disconnect(s_peer_bda);
    }
}

void BTAudio::reconnect() {
    if (s_a2d_conn_state != ESP_A2D_CONNECTION_STATE_DISCONNECTED) return;
    
    if (s_has_peer_bda) {
        Serial.println("[BTAudio] Reconnecting to saved peer address...");
        esp_a2d_source_connect(s_peer_bda);
    } else {
        startScan();
    }
}

bool BTAudio::isConnected() {
    return s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED;
}

bool BTAudio::isDisconnected() {
    return s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED;
}

bool BTAudio::isAnnouncementPlaying() {
    bool decoding = (mp3 && mp3->isRunning());
    bool rbHasData = (outBuf && outBuf->rb && (xRingbufferGetCurFreeSize(outBuf->rb) < 8192));
    return decoding || rbHasData;
}

void BTAudio::playAnnouncement(const char* filepath) {
    if (!SPIFFS.exists(filepath)) {
        Serial.printf("[BTAudio] Error: Announcement file %s not found in SPIFFS!\n", filepath);
        return;
    }
    
    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
    if (mp3) { delete mp3; mp3 = nullptr; }
    if (fileSource) { delete fileSource; fileSource = nullptr; }

    fileSource = new AudioFileSourceSPIFFS(filepath);
    mp3 = new AudioGeneratorMP3(s_mp3DecoderSpace, sizeof(s_mp3DecoderSpace));
    
    if (outBuf) {
        if (outBuf->rb) {
            size_t dummy_bytes;
            while (uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(outBuf->rb, &dummy_bytes, 0, 8192)) {
                vRingbufferReturnItem(outBuf->rb, item);
            }
        }
        outBuf->SetGain(2.5f);
        bool ok = mp3->begin(fileSource, outBuf);
        if (!ok) {
            Serial.printf("[BTAudio] Error: mp3->begin() failed for %s!\n", filepath);
            delete mp3; mp3 = nullptr;
            delete fileSource; fileSource = nullptr;
            return;
        }
        
        Serial.printf("[BTAudio] Playing announcement: %s (Started OK)\n", filepath);
        
        // Pre-fill ringbuffer with initial MP3 audio frames
        do {
            if (!mp3->loop()) {
                break;
            }
        } while (mp3 && mp3->isRunning() && outBuf && outBuf->rb && xRingbufferGetCurFreeSize(outBuf->rb) > 1024);
    }
}

void BTAudio::loop() {
    if (resetTimerPending) {
        resetTimerPending = false;
        if (getFadeFactor() < 1.0f || timerExpired) {
            Serial.println("[BTAudio] Lautstärke am Lautsprecher verändert während Ausblendung -> Sleep Timer neu gestartet!");
            timerExpired = false;
            resetTimer();
        }
    }

    if (toggleTimerPending) {
        toggleTimerPending = false;
        toggleTimer();
    }
    if (nextTrackPending) {
        nextTrackPending = false;
        nextNoiseTrack();
    }

    if (mp3 && mp3->isRunning()) {
        do {
            if (!mp3->loop()) {
                mp3->stop();
                delete mp3; mp3 = nullptr;
                delete fileSource; fileSource = nullptr;
                break;
            }
        } while (mp3 && mp3->isRunning() && outBuf && outBuf->rb && xRingbufferGetCurFreeSize(outBuf->rb) > 1024);
    } else if (mp3 && !mp3->isRunning()) {
        delete mp3; mp3 = nullptr;
        delete fileSource; fileSource = nullptr;
    }
    
    if (mediaReadyPending && (millis() - connectedTime > 1500)) {
        mediaReadyPending = false;
        Serial.println("[BTAudio] Deferred Media ready check...");
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    }
}

void BTAudio::nextNoiseTrack() {
    int t = noiseGen.getType();
    t = (t + 1) % NUM_NOISE_TYPES;
    noiseGen.setType(t);
    storage.saveLastNoiseType(t);
    
    isPaused = false; // Automatically resume play when track changes
    Serial.printf("[BTAudio] Rauschen gewechselt auf Typ: %d\n", t);
    
    String filename = String("/") + String(t) + ".mp3";
    playAnnouncement(filename.c_str());
}

void BTAudio::toggleTimer() {
    isPaused = false; // Automatically resume play when timer changes
    TimerState nextState;
    if (timerState == TIMER_30_MIN) {
        nextState = TIMER_60_MIN;
        Serial.println("[BTAudio] Timer umgeschaltet auf: 60 Minuten");
        playAnnouncement("/timer_60.mp3");
    } else if (timerState == TIMER_60_MIN) {
        nextState = TIMER_ENDLESS;
        Serial.println("[BTAudio] Timer umgeschaltet auf: Endlos");
        playAnnouncement("/timer_endless.mp3");
    } else {
        nextState = TIMER_30_MIN;
        Serial.println("[BTAudio] Timer umgeschaltet auf: 30 Minuten");
        playAnnouncement("/timer_30.mp3");
    }
    timerState = nextState;
    // We don't have saveLastTimerState in storage, we just rely on default timer
    resetTimer();
}

void BTAudio::resetTimer() {
    timerStartTime = millis();
    setFadeFactor(1.0f);
}
