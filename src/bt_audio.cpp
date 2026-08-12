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

// Audio processing
extern bool timerExpired;
class AudioOutputRingBuf : public AudioOutput {
public:
    RingbufHandle_t rb;
    AudioOutputRingBuf() {
        rb = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
    }
    ~AudioOutputRingBuf() {
        if (rb) vRingbufferDelete(rb);
    }
    virtual bool begin() override { return true; }
    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!rb) return false;
        xRingbufferSend(rb, sample, 4, pdMS_TO_TICKS(10));
        return true;
    }
    virtual bool stop() override { return true; }
};

static AudioFileSourceSPIFFS *fileSource = nullptr;
static AudioGeneratorMP3 *mp3 = nullptr;
static AudioOutputRingBuf *outBuf = nullptr;
static std::vector<ScannedDevice> s_foundDevices;
static std::vector<String> s_targetDevices;

String BTAudio::pendingDeviceName = "";

// -------------------------------------------------------------
// ESP-IDF Callbacks
// -------------------------------------------------------------
static int32_t audio_data_callback(uint8_t *data, int32_t len) {
    if (!data || len <= 0) return 0;
    
    if (btAudio.isPaused || timerExpired) {
        memset(data, 0, len);
        return len;
    }
    
    if (outBuf && outBuf->rb) {
        size_t bytes_received;
        uint8_t *rb_data = (uint8_t *)xRingbufferReceiveUpTo(outBuf->rb, &bytes_received, 0, len);
        if (rb_data && bytes_received > 0) {
            memcpy(data, rb_data, bytes_received);
            vRingbufferReturnItem(outBuf->rb, rb_data);
            if (bytes_received < len) {
                int32_t remaining = len - bytes_received;
                noiseGen.getFrames((NoiseGenerator::Frame*)(data + bytes_received), remaining / 4);
            }
            return len;
        }
    }
    
    noiseGen.getFrames((NoiseGenerator::Frame*)data, len / 4);
    return len;
}

static void bt_app_av_sm_hdlr(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
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
        
        if (param->conn_stat.connected) {
            esp_avrc_rn_evt_cap_mask_t evt_set = {0};
            esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_PLAY_STATUS_CHANGE);
            if (esp_avrc_tg_set_rn_evt_cap(&evt_set) != ESP_OK) {
                Serial.println("[BTAudio] esp_avrc_tg_set_rn_evt_cap failed in callback");
            }
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
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
        if (param->psth_cmd.key_state == 0) { // Pressed
            Serial.printf("[BTAudio] AVRCP PT_CMD %d\n", param->psth_cmd.key_code);
            if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_PLAY) {
                btAudio.isPaused = false;
                Serial.println("[BTAudio] -> Action: Play");
                if (millis() - btAudio.lastPauseTime < 1000) {
                    btAudio.toggleTimer();
                }
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_PAUSE) {
                btAudio.isPaused = true;
                btAudio.lastPauseTime = millis();
                Serial.println("[BTAudio] -> Action: Pause");
            } else if (param->psth_cmd.key_code == ESP_AVRC_PT_CMD_FORWARD) {
                btAudio.nextNoiseTrack();
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
            
            // Check if it's our target device
            for (const String& target : s_targetDevices) {
                if (target == name) {
                    ESP_LOGI(BT_AV_TAG, "Found target device %s! Connecting...", name.c_str());
                    esp_bt_gap_cancel_discovery();
                    memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
                    s_has_peer_bda = true;
                    esp_a2d_source_connect(s_peer_bda);
                    break;
                }
            }
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(BT_AV_TAG, "Discovery stopped.");
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(BT_AV_TAG, "Discovery started.");
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

    // AVRCP Setup
    esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
    esp_avrc_tg_init();
    
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
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
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
    if (s_has_peer_bda) {
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
    return (mp3 && mp3->isRunning());
}

void BTAudio::playAnnouncement(const char* filepath) {
    if (mp3 && mp3->isRunning()) {
        mp3->stop();
    }
    if (mp3) delete mp3;
    if (fileSource) delete fileSource;

    fileSource = new AudioFileSourceSPIFFS(filepath);
    mp3 = new AudioGeneratorMP3();
    
    if (outBuf) {
        mp3->begin(fileSource, outBuf);
    }
}

void BTAudio::loop() {
    if (mp3 && mp3->isRunning()) {
        if (!mp3->loop()) {
            mp3->stop();
            delete mp3; mp3 = nullptr;
            delete fileSource; fileSource = nullptr;
        }
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
    
    String filename = String("/") + String(t) + ".mp3";
    playAnnouncement(filename.c_str());
}

void BTAudio::toggleTimer() {
    TimerState nextState;
    if (timerState == TIMER_30_MIN) {
        nextState = TIMER_60_MIN;
        playAnnouncement("/60min.mp3");
    } else if (timerState == TIMER_60_MIN) {
        nextState = TIMER_ENDLESS;
        playAnnouncement("/endlos.mp3");
    } else {
        nextState = TIMER_30_MIN;
        playAnnouncement("/30min.mp3");
    }
    timerState = nextState;
    // We don't have saveLastTimerState in storage, we just rely on default timer
    resetTimer();
}

void BTAudio::resetTimer() {
    timerStartTime = millis();
}
