#ifndef DEVICE_IDENTITY_PRESETS_H
#define DEVICE_IDENTITY_PRESETS_H

#include <cstdint>
#include <nvs_flash.h>

/** NVS wifi namespace key (max 15 chars). 0 = chip/custom MAC, 1..5 = presets, 6 = manual custom MAC. */
#define NVS_KEY_PRESET_MAC_IDX "preset_mac"
#define NVS_KEY_CUSTOM_MAC "custom_mac"
#define NVS_KEY_PRESET_IDENTITY_LEGACY "preset_identity"
#define CUSTOM_MAC_STR_MAX 18

/** Stored preset_mac_idx: 0 or 6 use custom_mac NVS (optional for 0); 1..5 = named presets. */
inline constexpr uint8_t kMaxStoredPresetMacIndex = 6;

struct DeviceIdentityPreset {
    const char* name;
    const char* mac;
};

inline constexpr DeviceIdentityPreset kDeviceIdentityPresets[] = {
    {"explorers", "ba:53:9e:c5:fa:10"},
    {"younginnovators", "ba:53:9e:c5:fa:11"},
    {"futureleaders", "ba:53:9e:c5:fa:12"},
    {"ielts", "ba:53:9e:c5:fa:13"},
    {"toeic", "ba:53:9e:c5:fa:14"},
};

inline constexpr int kDeviceIdentityPresetCount =
    sizeof(kDeviceIdentityPresets) / sizeof(kDeviceIdentityPresets[0]);

/** Default when NVS has no preset_mac: 1 = explorers */
inline constexpr uint8_t kDefaultPresetMacIndex = 1;

/** @param idx 1..kDeviceIdentityPresetCount, or 0 for none */
inline const char* GetPresetMacByIndex(uint8_t idx) {
    if (idx == 0 || idx > kDeviceIdentityPresetCount) {
        return nullptr;
    }
    return kDeviceIdentityPresets[idx - 1].mac;
}

/** Read preset_mac index from NVS (i32 preferred; migrates legacy u8/bool). */
inline uint8_t ReadPresetMacIndexFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return kDefaultPresetMacIndex;
    }
    int32_t idx32 = 0;
    uint8_t idx = 0;
    bool found = false;
    if (nvs_get_i32(nvs, NVS_KEY_PRESET_MAC_IDX, &idx32) == ESP_OK) {
        idx = static_cast<uint8_t>(idx32);
        found = true;
    } else if (nvs_get_u8(nvs, NVS_KEY_PRESET_MAC_IDX, &idx) == ESP_OK) {
        found = true;
    } else {
        uint8_t legacy = 0;
        if (nvs_get_u8(nvs, NVS_KEY_PRESET_IDENTITY_LEGACY, &legacy) == ESP_OK && legacy != 0) {
            idx = kDefaultPresetMacIndex;
            found = true;
        }
    }
    nvs_close(nvs);
    if (!found) {
        return kDefaultPresetMacIndex;
    }
    if (idx > kMaxStoredPresetMacIndex) {
        return kDefaultPresetMacIndex;
    }
    return idx;
}

/** Read optional custom MAC (aa:bb:cc:dd:ee:ff) when preset_mac_idx is 0 or 6. */
inline bool ReadCustomMacFromNvs(char* out, size_t out_len) {
    if (out == nullptr || out_len < CUSTOM_MAC_STR_MAX) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(nvs, NVS_KEY_CUSTOM_MAC, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && out[0] != '\0';
}

#endif
