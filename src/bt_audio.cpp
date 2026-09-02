#include "bt_audio.h"
#include "noise_gen.h"
#include <WiFi.h>
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
        // Return true ONLY if sample was accepted into ringbuffer!
        // Returning false when full is required by ESP8266Audio to pause decoding at real-time rate.
        BaseType_t res = xRingbufferSend(rb, s, 4, 0);
        return (res == pdTRUE);
    }
    virtual bool stop() override { return true; }
};

static AudioFileSourceSPIFFS *fileSource = nullptr;
static AudioGeneratorMP3 *mp3 = nullptr;
static AudioOutputRingBuf *outBuf = nullptr;
static std::vector<ScannedDevice> s_foundDevices;
static std::vector<String> s_targetDevices;
static bool s_is_connecting = false;
static uint32_t s_pending_a2dp_connect_time = 0;
static uint32_t s_last_disconnect_time = 0;
static bool s_media_ctrl_in_flight = false;
static int s_consecutive_fails = 0;
static uint8_t s_current_volume = 100;
static int8_t s_current_rssi_delta = 0;

// Static preallocated buffer for MP3 decoder using exact library size requirement
static uint8_t s_mp3PreAlloc[AudioGeneratorMP3::preAllocSize()];

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
static portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;

static int32_t bt_app_a2d_data_cb(uint8_t *data, int32_t len) {
    if (len <= 0 || data == NULL) return 0;
    
    // Fill buffer with zeroes if paused or audio not ready
    if (btAudio.isPaused || timerExpired) {
        memset(data, 0, len);
        return len;
    }
    
    // 1. Check if there are MP3 announcement samples in the ringbuffer
    if (outBuf && outBuf->rb) {
        size_t bytes_received = 0;
        uint8_t *rb_data = (uint8_t *)xRingbufferReceiveUpTo(outBuf->rb, &bytes_received, 0, len);
        if (rb_data && bytes_received > 0) {
            memcpy(data, rb_data, bytes_received);
            vRingbufferReturnItem(outBuf->rb, rb_data);
            if (bytes_received < (size_t)len) {
                // If MP3 ended mid-packet, fill remaining with noise
                noiseGen.getFrames((NoiseGenerator::Frame*)(data + bytes_received), (len - bytes_received) / 4);
            }
            return len;
        }
    }
    
    // 2. Pure White Noise Generation
    noiseGen.getFrames((NoiseGenerator::Frame*)data, len / 4);
    
    // Apply Volume Fade & Overlay Warning Beep if active
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
    return len;
}

static void bt_app_av_sm_hdlr(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        s_a2d_conn_state = param->conn_stat.state;
        if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(BT_AV_TAG, ">>> A2DP CONNECTED SUCCESSFULLY! <<<");
            s_is_connecting = false;
            s_consecutive_fails = 0;
            memcpy(s_peer_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            s_has_peer_bda = true;
            btAudio.isPaused = false; // Default PLAY on connect
            btAudio.connectedTime = millis();
            
            // Critical for Coexistence: Disable BT discoverability while streaming.
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            
            // Ensure volume is at 100% and not muted
            btAudio.setFadeFactor(1.0f);
            
            // Fast Start: Check if media source channel is ready
            ESP_LOGD(BT_AV_TAG, "Triggering media ready check on connect...");
            s_media_ctrl_in_flight = true;
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            
            if (BTAudio::pendingDeviceName != "") {
                storage.addSavedDevice(BTAudio::pendingDeviceName);
                ESP_LOGI(BT_AV_TAG, "Successfully connected to %s. Saved!", BTAudio::pendingDeviceName.c_str());
                BTAudio::pendingDeviceName = "";
            }
        } else if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGW(BT_AV_TAG, "A2DP Disconnected (Reason: %d, Free Heap: %u)",
                     param->conn_stat.disc_rsn, (unsigned int)ESP.getFreeHeap());
            s_is_connecting = false;
            s_media_ctrl_in_flight = false;
            s_last_disconnect_time = millis();
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT: {
        s_a2d_audio_state = param->audio_stat.state;
        if (s_a2d_audio_state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(BT_AV_TAG, "🔊 A2DP Audio Streaming Started (White Noise Playing)");
        } else if (s_a2d_audio_state == ESP_A2D_AUDIO_STATE_STOPPED) {
            ESP_LOGD(BT_AV_TAG, "A2DP Audio Streaming Stopped");
        } else if (s_a2d_audio_state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
            ESP_LOGD(BT_AV_TAG, "A2DP Audio Streaming Suspended (Remote)");
        }
        break;
    }
    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        s_media_ctrl_in_flight = false;
        ESP_LOGD(BT_AV_TAG, "A2DP media ctrl ack: cmd %d, status %d", param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "Media ready -> Triggering A2DP stream START...");
                s_media_ctrl_in_flight = true;
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            }
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_START) {
            if (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(BT_AV_TAG, "A2DP stream start acknowledged by peer!");
            }
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
        ESP_LOGI(BT_AV_TAG, "AVRC TG conn_state evt: state %d, [%02x:%02x:%02x:%02x:%02x:%02x]",
                 param->conn_stat.connected, bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        if (param->conn_stat.connected == 1 && s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED && !s_is_connecting) {
            memcpy(s_peer_bda, bda, ESP_BD_ADDR_LEN);
            s_has_peer_bda = true;
            // Initiate A2DP quickly after AVRCP (400ms delay to allow link settling without long wait)
            s_pending_a2dp_connect_time = millis() + 400;
        }
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
        static uint8_t last_vol = 0;
        uint8_t current_vol = param->set_abs_vol.volume;
        s_current_volume = current_vol;
        ESP_LOGI(BT_AV_TAG, "AVRCP TG Lautstärke: %d/127 (%.0f%%)", current_vol, (current_vol * 100.0f) / 127.0f);
        btAudio.resetTimerPending = true;
        
        // Auto-Play if volume is increased while paused
        if (btAudio.isPaused && current_vol > last_vol) {
            ESP_LOGI(BT_AV_TAG, "Lautstärke erhöht -> Auto Play!");
            btAudio.isPaused = false;
        }
        last_vol = current_vol;
        break;
    }
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
        if (param->psth_cmd.key_state == 0) { // Pressed
            ESP_LOGI(BT_AV_TAG, "AVRCP PT_CMD: %d", param->psth_cmd.key_code);
            if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_PLAY) {
                btAudio.isPaused = false;
                ESP_LOGI(BT_AV_TAG, "-> Action: Play (Unmuted)");
                // Software double-click fallback
                if (millis() - btAudio.lastPauseTime < 1000) {
                    ESP_LOGI(BT_AV_TAG, "-> Software Double-Click Detected!");
                    btAudio.toggleTimerPending = true;
                }
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_PAUSE) {
                btAudio.isPaused = true;
                btAudio.lastPauseTime = millis();
                ESP_LOGI(BT_AV_TAG, "-> Action: Pause (Muted)");
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_FORWARD) {
                ESP_LOGI(BT_AV_TAG, "-> Action: Forward (Hardware Double-Click!)");
                btAudio.toggleTimerPending = true;
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_BACKWARD) {
                ESP_LOGI(BT_AV_TAG, "-> Action: Backward (Hardware Triple-Click!)");
                btAudio.nextTrackPending = true;
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_UP) {
                ESP_LOGI(BT_AV_TAG, "-> Volume UP Button Pressed on Speaker!");
                btAudio.resetTimerPending = true;
                if (btAudio.isPaused) {
                    ESP_LOGI(BT_AV_TAG, "Volume UP while paused -> Auto Play!");
                    btAudio.isPaused = false;
                }
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_VOL_DOWN) {
                ESP_LOGI(BT_AV_TAG, "-> Volume DOWN Button Pressed on Speaker!");
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
        ESP_LOGI(BT_AV_TAG, "AVRC CT conn_state evt: state %d", param->conn_stat.connected);
    } else if (event == ESP_AVRC_CT_CHANGE_NOTIFY_EVT) {
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            s_current_volume = param->change_ntf.event_parameter.volume;
            ESP_LOGI(BT_AV_TAG, "AVRCP CT Lautstärke-Änderung: %d/127 (%.0f%%)", s_current_volume, (s_current_volume * 100.0f) / 127.0f);
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
            if (!updated && s_foundDevices.size() < 10) {
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
            if (s_pending_connect && s_has_peer_bda && s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED && !s_is_connecting) {
                s_pending_connect = false;
                s_is_connecting = true;
                Serial.println("[BTAudio] Initiating A2DP connection to target device...");
                esp_a2d_source_connect(s_peer_bda);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            Serial.println("[BTAudio] Discovery started.");
            s_is_discovering = true;
        }
        break;
    }
    case ESP_BT_GAP_READ_RSSI_DELTA_EVT: {
        s_current_rssi_delta = param->read_rssi_delta.rssi_delta;
        ESP_LOGI(BT_AV_TAG, "RSSI Delta zum Lautsprecher: %d dB", s_current_rssi_delta);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT: {
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_CFM_REQ_EVT: auto-confirming pairing with [%02x:%02x:%02x:%02x:%02x:%02x]",
                 param->cfm_req.bda[0], param->cfm_req.bda[1], param->cfm_req.bda[2],
                 param->cfm_req.bda[3], param->cfm_req.bda[4], param->cfm_req.bda[5]);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    }
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey: %u", (unsigned int)param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_KEY_REQ_EVT");
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(BT_AV_TAG, "Pairing / Authentication SUCCESS: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(BT_AV_TAG, "Pairing / Authentication FAILED, status: %d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_PIN_REQ_EVT: {
        ESP_LOGI(BT_AV_TAG, "ESP_BT_GAP_PIN_REQ_EVT: Replying with pin 0000");
        esp_bt_pin_code_t pin_code = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
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
    
    if (storage.getSavedMac(s_peer_bda)) {
        s_has_peer_bda = true;
        Serial.printf("[BTAudio] Loaded saved MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
                      s_peer_bda[0], s_peer_bda[1], s_peer_bda[2], s_peer_bda[3], s_peer_bda[4], s_peer_bda[5]);
    }
    
    String pending = storage.popPendingDevice();
    s_targetDevices = savedDevices;
    if (pending != "") {
        BTAudio::pendingDeviceName = pending;
        if (std::find(s_targetDevices.begin(), s_targetDevices.end(), pending) == s_targetDevices.end()) {
            s_targetDevices.push_back(pending);
        }
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
    esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);
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
    storage.addSavedDevice(name);
    if (std::find(s_targetDevices.begin(), s_targetDevices.end(), name) == s_targetDevices.end()) {
        s_targetDevices.push_back(name);
    }
    
    // Check if the target device is already in s_foundDevices list from scanning
    for (auto& dev : s_foundDevices) {
        if (dev.name.equalsIgnoreCase(name) || name.indexOf(dev.name) >= 0 || dev.name.indexOf(name) >= 0) {
            Serial.printf("[BTAudio] Instant connect to scanned device '%s' (%02x:%02x:%02x:%02x:%02x:%02x)!\n", 
                          dev.name.c_str(), dev.bda[0], dev.bda[1], dev.bda[2], dev.bda[3], dev.bda[4], dev.bda[5]);
            memcpy(s_peer_bda, dev.bda, ESP_BD_ADDR_LEN);
            s_has_peer_bda = true;
            storage.saveSavedMac(s_peer_bda);
            
            if (s_is_discovering) {
                esp_bt_gap_cancel_discovery();
            }
            if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED && !s_is_connecting) {
                s_is_connecting = true;
                Serial.println("[BTAudio] Initiating direct A2DP connection immediately...");
                esp_a2d_source_connect(s_peer_bda);
            }
            return;
        }
    }
    
    // Not found in active scan list yet, trigger discovery scan
    startScan();
}

void BTAudio::disconnect() {
    if (isConnected()) {
        esp_a2d_source_disconnect(s_peer_bda);
    }
}

void BTAudio::reconnect() {
    static uint32_t lastConnectAttempt = 0;
    if (s_a2d_conn_state != ESP_A2D_CONNECTION_STATE_DISCONNECTED || s_is_connecting || s_is_discovering) return;
    
    // Fast start on boot: 300ms to let BT baseband stabilize, then connect immediately!
    if (lastConnectAttempt == 0 && millis() < 300) return;
    
    // Wait at least 3 seconds AFTER the last disconnect before retrying
    if (s_last_disconnect_time > 0 && millis() - s_last_disconnect_time < 3000) return;

    // Gentle backoff when speaker is off to preserve memory and prevent Bluedroid timer crashes
    uint32_t retryInterval = 5000;
    if (s_consecutive_fails > 10) {
        retryInterval = 15000; // 15s if speaker is off for a while
    } else if (s_consecutive_fails > 3) {
        retryInterval = 8000;  // 8s after 3 failed attempts
    }

    if (millis() - lastConnectAttempt < retryInterval) return;
    lastConnectAttempt = millis();

    if (s_has_peer_bda) {
        s_consecutive_fails++;
        s_is_connecting = true;
        ESP_LOGI(BT_AV_TAG, "Reconnecting to saved peer address [%02x:%02x:%02x:%02x:%02x:%02x] (Attempt %d, Free Heap: %u)...",
                 s_peer_bda[0], s_peer_bda[1], s_peer_bda[2], s_peer_bda[3], s_peer_bda[4], s_peer_bda[5],
                 s_consecutive_fails, (unsigned int)ESP.getFreeHeap());
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
    return (mp3 != nullptr && mp3->isRunning());
}

void BTAudio::playAnnouncement(const char* filepath) {
    if (!isConnected()) {
        ESP_LOGW(BT_AV_TAG, "Skipping announcement %s (Bluetooth not connected).", filepath);
        return;
    }

    if (!SPIFFS.exists(filepath)) {
        ESP_LOGE(BT_AV_TAG, "Announcement file %s not found in SPIFFS!", filepath);
        return;
    }
    
    File f = SPIFFS.open(filepath, "r");
    size_t fsize = f ? f.size() : 0;
    if (f) f.close();
    ESP_LOGI(BT_AV_TAG, "Starting announcement: %s (Size: %u bytes)", filepath, (unsigned int)fsize);

    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
    if (mp3) { delete mp3; mp3 = nullptr; }
    if (fileSource) { delete fileSource; fileSource = nullptr; }

    if (outBuf && outBuf->rb) {
        size_t dummy_bytes;
        while (uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(outBuf->rb, &dummy_bytes, 0, 8192)) {
            vRingbufferReturnItem(outBuf->rb, item);
        }
    }

    fileSource = new AudioFileSourceSPIFFS(filepath);
    mp3 = new AudioGeneratorMP3(s_mp3PreAlloc, sizeof(s_mp3PreAlloc));
    
    if (outBuf) {
        outBuf->SetGain(2.5f);
        bool ok = mp3->begin(fileSource, outBuf);
        if (!ok) {
            ESP_LOGE(BT_AV_TAG, "mp3->begin() failed for %s!", filepath);
            delete mp3; mp3 = nullptr;
            delete fileSource; fileSource = nullptr;
            return;
        }
        
        ESP_LOGI(BT_AV_TAG, "Announcement started successfully: %s", filepath);
        
        // Pre-fill ringbuffer with initial MP3 audio frames
        int prefillCount = 0;
        while (mp3 && mp3->isRunning() && outBuf && outBuf->rb && xRingbufferGetCurFreeSize(outBuf->rb) > 2048 && prefillCount++ < 15) {
            if (!mp3->loop()) {
                break;
            }
        }
    }
}

void BTAudio::loop() {
    if (resetTimerPending) {
        resetTimerPending = false;
        if (getFadeFactor() < 1.0f || timerExpired) {
            ESP_LOGI(BT_AV_TAG, "Volume changed during fade-out -> Sleep timer restarted!");
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
        // Feed MP3 decoder while there is free space in the ringbuffer
        int loops = 0;
        while (mp3->isRunning() && outBuf && outBuf->rb && xRingbufferGetCurFreeSize(outBuf->rb) > 1024 && loops++ < 20) {
            if (!mp3->loop()) {
                ESP_LOGI(BT_AV_TAG, "MP3 Announcement decode finished.");
                mp3->stop();
                delete mp3; mp3 = nullptr;
                delete fileSource; fileSource = nullptr;
                break;
            }
        }
    } else if (mp3 && !mp3->isRunning()) {
        delete mp3; mp3 = nullptr;
        delete fileSource; fileSource = nullptr;
    }
    
    if (s_pending_a2dp_connect_time > 0 && millis() >= s_pending_a2dp_connect_time) {
        s_pending_a2dp_connect_time = 0;
        if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_DISCONNECTED && !s_is_connecting) {
            s_is_connecting = true;
            ESP_LOGI(BT_AV_TAG, "AVRCP link settled. Initiating A2DP audio connection now...");
            esp_a2d_source_connect(s_peer_bda);
        }
    }

    // Regelmäßige Status- und Pegelausgabe alle 10 Sekunden
    static uint32_t lastStatusPrint = 0;
    if (millis() - lastStatusPrint >= 10000) {
        lastStatusPrint = millis();
        if (s_a2d_conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            esp_bt_gap_read_rssi_delta(s_peer_bda);
            
            const char* timerStr = "Endlos";
            char timerBuf[32];
            if (timerState != TIMER_ENDLESS) {
                uint32_t dur = (timerState == TIMER_30_MIN) ? 30 * 60 : 60 * 60;
                uint32_t elapsed = (millis() - timerStartTime) / 1000;
                uint32_t rem = (dur > elapsed) ? (dur - elapsed) : 0;
                snprintf(timerBuf, sizeof(timerBuf), "%um %02us rest", (unsigned int)(rem / 60), (unsigned int)(rem % 60));
                timerStr = timerBuf;
            }
            
            ESP_LOGI(BT_AV_TAG, "[Status] 🔊 %s | Pegel: %d%% (%d/127) | RSSI-Delta: %d dB | %s | Timer: %s | Heap: %u KB",
                     isPaused ? "PAUSE" : "PLAYING",
                     (int)((s_current_volume * 100) / 127),
                     s_current_volume,
                     s_current_rssi_delta,
                     noiseGen.getTypeName(noiseGen.getType()),
                     timerStr,
                     (unsigned int)(ESP.getFreeHeap() / 1024));
        }
    }
}

void BTAudio::nextNoiseTrack() {
    int t = noiseGen.getType();
    t = (t + 1) % NUM_NOISE_TYPES;
    noiseGen.setType(t);
    storage.saveLastNoiseType(t);
    
    isPaused = false; // Automatically resume play when track changes
    ESP_LOGI(BT_AV_TAG, "Rauschen gewechselt auf Typ: %d (%s)", t, noiseGen.getTypeName(t));
    
    String filename = String("/") + String(t) + ".mp3";
    if (SPIFFS.exists(filename.c_str())) {
        playAnnouncement(filename.c_str());
    } else {
        ESP_LOGW(BT_AV_TAG, "Announcement file %s not found -> playing beep fallback", filename.c_str());
        triggerWarningBeep();
    }
}

void BTAudio::toggleTimer() {
    isPaused = false; // Automatically resume play when timer changes
    TimerState nextState;
    if (timerState == TIMER_30_MIN) {
        nextState = TIMER_60_MIN;
        ESP_LOGI(BT_AV_TAG, "Timer umgeschaltet auf: 60 Minuten");
        if (SPIFFS.exists("/timer_60.mp3")) {
            playAnnouncement("/timer_60.mp3");
        } else {
            ESP_LOGW(BT_AV_TAG, "Announcement /timer_60.mp3 not found -> playing beep fallback");
            triggerWarningBeep();
        }
    } else if (timerState == TIMER_60_MIN) {
        nextState = TIMER_ENDLESS;
        ESP_LOGI(BT_AV_TAG, "Timer umgeschaltet auf: Endlos");
        if (SPIFFS.exists("/timer_endless.mp3")) {
            playAnnouncement("/timer_endless.mp3");
        } else {
            ESP_LOGW(BT_AV_TAG, "Announcement /timer_endless.mp3 not found -> playing beep fallback");
            triggerWarningBeep();
        }
    } else {
        nextState = TIMER_30_MIN;
        ESP_LOGI(BT_AV_TAG, "Timer umgeschaltet auf: 30 Minuten");
        if (SPIFFS.exists("/timer_30.mp3")) {
            playAnnouncement("/timer_30.mp3");
        } else {
            ESP_LOGW(BT_AV_TAG, "Announcement /timer_30.mp3 not found -> playing beep fallback");
            triggerWarningBeep();
        }
    }
    timerState = nextState;
    resetTimer();
}

bool BTAudio::isStreaming() {
    return (s_a2d_audio_state == ESP_A2D_AUDIO_STATE_STARTED);
}


void BTAudio::resetTimer() {
    timerStartTime = millis();
    setFadeFactor(1.0f);
}
