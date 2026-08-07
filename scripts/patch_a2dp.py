import os
Import("env")

def patch_a2dp(source, target, env):
    lib_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV/ESP32-A2DP")
    file_path = os.path.join(lib_dir, "src", "BluetoothA2DPSource.cpp")
    
    if not os.path.isfile(file_path):
        return

    with open(file_path, "r") as f:
        content = f.read()

    # Apply the patch if not already applied
    search_str = 'if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {\n        ESP_LOGW(BT_AV_TAG, "ESP_AVRC_RN_VOLUME_CHANGE");\n        // s_volume_notify = true;\n      }'
    
    replace_str = '''if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        ESP_LOGW(BT_AV_TAG, "ESP_AVRC_RN_VOLUME_CHANGE");
        // s_volume_notify = true;
      } else if (rc->reg_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
        ESP_LOGW(BT_AV_TAG, "ESP_AVRC_RN_PLAY_STATUS_CHANGE requested!");
        esp_avrc_rn_param_t rn_param;
        rn_param.play_status = ESP_AVRC_PLAYBACK_PLAYING;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
      }'''

    if search_str in content and replace_str not in content:
        content = content.replace(search_str, replace_str)
        with open(file_path, "w") as f:
            f.write(content)
        print("Patched ESP32-A2DP library to support PLAY_STATUS_CHANGE")

env.AddPreAction("buildprog", patch_a2dp)
