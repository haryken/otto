#ifndef DEVICE_IDENTITY_PRESETS_H
#define DEVICE_IDENTITY_PRESETS_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <esp_random.h>
#include <nvs_flash.h>

/** NVS wifi namespace key (max 15 chars).
 *  0 = chip/custom MAC, 1..5 = named course presets,
 *  6 = manual custom MAC, 7 = daily chat (random from pool on boot). */
#define NVS_KEY_PRESET_MAC_IDX "preset_mac"
#define NVS_KEY_CUSTOM_MAC "custom_mac"
#define NVS_KEY_PRESET_IDENTITY_LEGACY "preset_identity"
#define CUSTOM_MAC_STR_MAX 18

inline constexpr uint8_t kManualCustomMacIndex = 6;
inline constexpr uint8_t kDailyChatPresetMacIndex = 7;
/** Stored preset_mac_idx: 0..7 */
inline constexpr uint8_t kMaxStoredPresetMacIndex = 7;

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

/** 20 MACs for "Giao tiếp hằng ngày" — one picked at random each boot. */
inline constexpr const char* kDailyChatMacPool[] = {
    "1c:db:d4:b5:73:3c",
    "58:a0:23:a6:fe:31",
    "a8:b5:44:dd:e3:cf",
    "1c:db:d4:b5:74:7c",
    "1c:db:d4:b5:6a:d8",
    "1c:db:d4:b5:74:54",
    "1c:db:d4:b5:72:ec",
    "1c:db:d4:b5:71:d4",
    "1c:db:d4:b5:74:d4",
    "1c:db:d4:a9:48:84",
    "1c:db:d4:a9:59:b4",
    "1c:db:d4:a9:5b:a0",
    "28:df:eb:02:6c:7d",
    "bc:fc:e7:8a:d8:06",
    "dc:b4:d9:0c:a4:9c",
    "dc:b4:d9:0c:a4:80",
    "dc:b4:d9:0c:a6:00",
    "dc:b4:d9:03:4e:f4",
    "dc:b4:d9:0c:a5:38",
    "dc:b4:d9:03:43:38",
};

inline constexpr int kDailyChatMacPoolCount =
    sizeof(kDailyChatMacPool) / sizeof(kDailyChatMacPool[0]);

/** Default when NVS has no preset_mac: 1 = explorers */
inline constexpr uint8_t kDefaultPresetMacIndex = 1;

/** @param idx 1..kDeviceIdentityPresetCount, or 0 for none */
inline const char* GetPresetMacByIndex(uint8_t idx) {
    if (idx == 0 || idx > kDeviceIdentityPresetCount) {
        return nullptr;
    }
    return kDeviceIdentityPresets[idx - 1].mac;
}

inline bool UsesCustomMacNvs(uint8_t idx) {
    return idx == 0 || idx == kManualCustomMacIndex || idx == kDailyChatPresetMacIndex;
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

/** Read optional custom MAC (aa:bb:cc:dd:ee:ff) when preset_mac_idx is 0, 6, or 7. */
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

inline bool WriteCustomMacToNvs(const char* mac) {
    if (mac == nullptr) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err;
    if (mac[0] == '\0') {
        nvs_erase_key(nvs, NVS_KEY_CUSTOM_MAC);
        err = ESP_OK;
    } else {
        err = nvs_set_str(nvs, NVS_KEY_CUSTOM_MAC, mac);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

/** Pick one MAC from the daily pool and store it in custom_mac NVS. */
inline const char* PickAndSaveDailyChatMac() {
    if (kDailyChatMacPoolCount <= 0) {
        return nullptr;
    }
    uint32_t r = esp_random();
    int i = static_cast<int>(r % static_cast<uint32_t>(kDailyChatMacPoolCount));
    const char* mac = kDailyChatMacPool[i];
    WriteCustomMacToNvs(mac);
    return mac;
}

/**
 * Call once early after NVS init (each boot).
 * If course is "Giao tiếp hằng ngày", randomize Device-Id from the 20-MAC pool.
 */
inline void ApplyDailyChatIdentityOnBoot() {
    if (ReadPresetMacIndexFromNvs() != kDailyChatPresetMacIndex) {
        return;
    }
    const char* mac = PickAndSaveDailyChatMac();
    if (mac != nullptr) {
        // Logging via ESP_LOGI needs a TAG; keep silent here — callers may log.
        (void)mac;
    }
}

#endif
