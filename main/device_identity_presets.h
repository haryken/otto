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

/** Default when NVS has no preset_mac: 1 = explorers (same value as kExplorersCourseIdx in otto_course_units.h). */
inline constexpr uint8_t kDefaultPresetMacIndex = 1;

struct DeviceIdentityPreset {
    const char* name;
    const char* mac;
};

inline constexpr DeviceIdentityPreset kDeviceIdentityPresets[] = {
    {"explorers", "ba:53:9e:c5:ba:10"},
    {"younginnovators", "ba:53:9e:c5:fa:11"},
    {"futureleaders", "ba:53:9e:c5:fa:12"},
    {"ielts", "ba:53:9e:c2:ef:10"},
    {"toeic", "ba:53:9e:c9:ef:10"},
};

inline constexpr int kDeviceIdentityPresetCount =
    sizeof(kDeviceIdentityPresets) / sizeof(kDeviceIdentityPresets[0]);

/**
 * Explorers voice / MAC pools — random pick into custom_mac on boot / switch.
 * 0 = Song ngữ Anh–Việt (ba:10–ba:19), 1 = English (fe:10–fe:19).
 */
#define NVS_KEY_EXPLORERS_VOICE "ex_voice"
inline constexpr uint8_t kExplorersVoiceBilingual = 0;
inline constexpr uint8_t kExplorersVoiceEnglish = 1;

inline constexpr const char* kExplorersBilingualMacPool[] = {
    "ba:53:9e:c5:ba:10",
    "ba:53:9e:c5:ba:11",
    "ba:53:9e:c5:ba:12",
    "ba:53:9e:c5:ba:13",
    "ba:53:9e:c5:ba:14",
    "ba:53:9e:c5:ba:15",
    "ba:53:9e:c5:ba:16",
    "ba:53:9e:c5:ba:17",
    "ba:53:9e:c5:ba:18",
    "ba:53:9e:c5:ba:19",
};

inline constexpr int kExplorersBilingualMacPoolCount =
    sizeof(kExplorersBilingualMacPool) / sizeof(kExplorersBilingualMacPool[0]);

inline constexpr const char* kExplorersEnglishMacPool[] = {
    "ba:53:9e:c5:fe:10",
    "ba:53:9e:c5:fe:11",
    "ba:53:9e:c5:fe:12",
    "ba:53:9e:c5:fe:13",
    "ba:53:9e:c5:fe:14",
    "ba:53:9e:c5:fe:15",
    "ba:53:9e:c5:fe:16",
    "ba:53:9e:c5:fe:17",
    "ba:53:9e:c5:fe:18",
    "ba:53:9e:c5:fe:19",
};

inline constexpr int kExplorersEnglishMacPoolCount =
    sizeof(kExplorersEnglishMacPool) / sizeof(kExplorersEnglishMacPool[0]);

/** @deprecated alias — bilingual pool (UI preset display). */
inline constexpr const char* const* kExplorersMacPool = kExplorersBilingualMacPool;
inline constexpr int kExplorersMacPoolCount = kExplorersBilingualMacPoolCount;

/**
 * Young Innovators voice / MAC pools (independent from Explorers).
 * 0 = Tiếng Việt (ef:10–ef:19), 1 = Tiếng Anh (c6:fe:10–fe:19).
 */
#define NVS_KEY_YI_VOICE "yi_voice"
/** Same value as kYoungInnovatorsCourseIdx in otto_course_units.h (do not redefine that name here). */
inline constexpr uint8_t kYoungInnovatorsPresetMacIndex = 2;
inline constexpr uint8_t kYiVoiceVietnamese = 0;
inline constexpr uint8_t kYiVoiceEnglish = 1;

inline constexpr const char* kYiVietnameseMacPool[] = {
    "ba:53:9e:c5:ef:10",
    "ba:53:9e:c5:ef:11",
    "ba:53:9e:c5:ef:12",
    "ba:53:9e:c5:ef:13",
    "ba:53:9e:c5:ef:14",
    "ba:53:9e:c5:ef:15",
    "ba:53:9e:c5:ef:16",
    "ba:53:9e:c5:ef:17",
    "ba:53:9e:c5:ef:18",
    "ba:53:9e:c5:ef:19",
};

inline constexpr int kYiVietnameseMacPoolCount =
    sizeof(kYiVietnameseMacPool) / sizeof(kYiVietnameseMacPool[0]);

inline constexpr const char* kYiEnglishMacPool[] = {
    "ba:53:9e:c6:fe:10",
    "ba:53:9e:c6:fe:11",
    "ba:53:9e:c6:fe:12",
    "ba:53:9e:c6:fe:13",
    "ba:53:9e:c6:fe:14",
    "ba:53:9e:c6:fe:15",
    "ba:53:9e:c6:fe:16",
    "ba:53:9e:c6:fe:17",
    "ba:53:9e:c6:fe:18",
    "ba:53:9e:c6:fe:19",
};

inline constexpr int kYiEnglishMacPoolCount =
    sizeof(kYiEnglishMacPool) / sizeof(kYiEnglishMacPool[0]);

/**
 * Future Leaders voice / MAC pools (independent from Explorers / YI).
 * 0 = Tiếng Việt (c7:ef:10–19), 1 = Tiếng Anh (c8:ef:10–19).
 */
#define NVS_KEY_FL_VOICE "fl_voice"
/** Same value as kFutureLeadersCourseIdx in otto_course_units.h (do not redefine that name). */
inline constexpr uint8_t kFutureLeadersPresetMacIndex = 3;
inline constexpr uint8_t kFlVoiceVietnamese = 0;
inline constexpr uint8_t kFlVoiceEnglish = 1;

inline constexpr const char* kFlVietnameseMacPool[] = {
    "ba:53:9e:c7:ef:10",
    "ba:53:9e:c7:ef:11",
    "ba:53:9e:c7:ef:12",
    "ba:53:9e:c7:ef:13",
    "ba:53:9e:c7:ef:14",
    "ba:53:9e:c7:ef:15",
    "ba:53:9e:c7:ef:16",
    "ba:53:9e:c7:ef:17",
    "ba:53:9e:c7:ef:18",
    "ba:53:9e:c7:ef:19",
};

inline constexpr int kFlVietnameseMacPoolCount =
    sizeof(kFlVietnameseMacPool) / sizeof(kFlVietnameseMacPool[0]);

inline constexpr const char* kFlEnglishMacPool[] = {
    "ba:53:9e:c8:ef:10",
    "ba:53:9e:c8:ef:11",
    "ba:53:9e:c8:ef:12",
    "ba:53:9e:c8:ef:13",
    "ba:53:9e:c8:ef:14",
    "ba:53:9e:c8:ef:15",
    "ba:53:9e:c8:ef:16",
    "ba:53:9e:c8:ef:17",
    "ba:53:9e:c8:ef:18",
    "ba:53:9e:c8:ef:19",
};

inline constexpr int kFlEnglishMacPoolCount =
    sizeof(kFlEnglishMacPool) / sizeof(kFlEnglishMacPool[0]);

/**
 * IELTS voice / MAC pools.
 * 0 = Tiếng Việt (c2:ef:10–19), 1 = Tiếng Anh (c3:ef:10–19).
 */
#define NVS_KEY_IELTS_VOICE "ielts_voice"
inline constexpr uint8_t kIeltsPresetMacIndex = 4;
inline constexpr uint8_t kIeltsVoiceVietnamese = 0;
inline constexpr uint8_t kIeltsVoiceEnglish = 1;

inline constexpr const char* kIeltsVietnameseMacPool[] = {
    "ba:53:9e:c2:ef:10",
    "ba:53:9e:c2:ef:11",
    "ba:53:9e:c2:ef:12",
    "ba:53:9e:c2:ef:13",
    "ba:53:9e:c2:ef:14",
    "ba:53:9e:c2:ef:15",
    "ba:53:9e:c2:ef:16",
    "ba:53:9e:c2:ef:17",
    "ba:53:9e:c2:ef:18",
    "ba:53:9e:c2:ef:19",
};

inline constexpr int kIeltsVietnameseMacPoolCount =
    sizeof(kIeltsVietnameseMacPool) / sizeof(kIeltsVietnameseMacPool[0]);

inline constexpr const char* kIeltsEnglishMacPool[] = {
    "ba:53:9e:c3:ef:10",
    "ba:53:9e:c3:ef:11",
    "ba:53:9e:c3:ef:12",
    "ba:53:9e:c3:ef:13",
    "ba:53:9e:c3:ef:14",
    "ba:53:9e:c3:ef:15",
    "ba:53:9e:c3:ef:16",
    "ba:53:9e:c3:ef:17",
    "ba:53:9e:c3:ef:18",
    "ba:53:9e:c3:ef:19",
};

inline constexpr int kIeltsEnglishMacPoolCount =
    sizeof(kIeltsEnglishMacPool) / sizeof(kIeltsEnglishMacPool[0]);

/**
 * TOEIC voice / MAC pools.
 * 0 = Tiếng Việt (c9:ef:10–19), 1 = Tiếng Anh (c1:ef:10–19).
 */
#define NVS_KEY_TOEIC_VOICE "toeic_voice"
inline constexpr uint8_t kToeicPresetMacIndex = 5;
inline constexpr uint8_t kToeicVoiceVietnamese = 0;
inline constexpr uint8_t kToeicVoiceEnglish = 1;

inline constexpr const char* kToeicVietnameseMacPool[] = {
    "ba:53:9e:c9:ef:10",
    "ba:53:9e:c9:ef:11",
    "ba:53:9e:c9:ef:12",
    "ba:53:9e:c9:ef:13",
    "ba:53:9e:c9:ef:14",
    "ba:53:9e:c9:ef:15",
    "ba:53:9e:c9:ef:16",
    "ba:53:9e:c9:ef:17",
    "ba:53:9e:c9:ef:18",
    "ba:53:9e:c9:ef:19",
};

inline constexpr int kToeicVietnameseMacPoolCount =
    sizeof(kToeicVietnameseMacPool) / sizeof(kToeicVietnameseMacPool[0]);

inline constexpr const char* kToeicEnglishMacPool[] = {
    "ba:53:9e:c1:ef:10",
    "ba:53:9e:c1:ef:11",
    "ba:53:9e:c1:ef:12",
    "ba:53:9e:c1:ef:13",
    "ba:53:9e:c1:ef:14",
    "ba:53:9e:c1:ef:15",
    "ba:53:9e:c1:ef:16",
    "ba:53:9e:c1:ef:17",
    "ba:53:9e:c1:ef:18",
    "ba:53:9e:c1:ef:19",
};

inline constexpr int kToeicEnglishMacPoolCount =
    sizeof(kToeicEnglishMacPool) / sizeof(kToeicEnglishMacPool[0]);

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

/** Courses that pick Device-Id from a MAC pool into custom_mac (boot + switch). */
inline bool CourseUsesMacPool(uint8_t idx) {
    return idx == kDefaultPresetMacIndex || idx == kYoungInnovatorsPresetMacIndex ||
           idx == kFutureLeadersPresetMacIndex || idx == kIeltsPresetMacIndex ||
           idx == kToeicPresetMacIndex || idx == kDailyChatPresetMacIndex;
}

/** @param idx 1..kDeviceIdentityPresetCount, or 0 for none */
inline const char* GetPresetMacByIndex(uint8_t idx) {
    if (idx == 0 || idx > kDeviceIdentityPresetCount) {
        return nullptr;
    }
    return kDeviceIdentityPresets[idx - 1].mac;
}

inline bool UsesCustomMacNvs(uint8_t idx) {
    return idx == 0 || idx == kManualCustomMacIndex || CourseUsesMacPool(idx);
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

inline const char* PickAndSaveFromPool(const char* const* pool, int count) {
    if (pool == nullptr || count <= 0) {
        return nullptr;
    }
    uint32_t r = esp_random();
    int i = static_cast<int>(r % static_cast<uint32_t>(count));
    const char* mac = pool[i];
    WriteCustomMacToNvs(mac);
    return mac;
}

inline uint8_t ReadExplorersVoiceFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return kExplorersVoiceBilingual;
    }
    int32_t voice = kExplorersVoiceBilingual;
    nvs_get_i32(nvs, NVS_KEY_EXPLORERS_VOICE, &voice);
    nvs_close(nvs);
    if (voice != kExplorersVoiceEnglish) {
        return kExplorersVoiceBilingual;
    }
    return kExplorersVoiceEnglish;
}

inline bool WriteExplorersVoiceToNvs(uint8_t voice) {
    if (voice != kExplorersVoiceEnglish) {
        voice = kExplorersVoiceBilingual;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_i32(nvs, NVS_KEY_EXPLORERS_VOICE, static_cast<int32_t>(voice));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

/** Pick one MAC from the active Explorers voice pool into custom_mac NVS. */
inline const char* PickAndSaveExplorersMac() {
    if (ReadExplorersVoiceFromNvs() == kExplorersVoiceEnglish) {
        return PickAndSaveFromPool(kExplorersEnglishMacPool, kExplorersEnglishMacPoolCount);
    }
    return PickAndSaveFromPool(kExplorersBilingualMacPool, kExplorersBilingualMacPoolCount);
}

inline uint8_t ReadYiVoiceFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return kYiVoiceVietnamese;
    }
    int32_t voice = kYiVoiceVietnamese;
    nvs_get_i32(nvs, NVS_KEY_YI_VOICE, &voice);
    nvs_close(nvs);
    if (voice != kYiVoiceEnglish) {
        return kYiVoiceVietnamese;
    }
    return kYiVoiceEnglish;
}

inline bool WriteYiVoiceToNvs(uint8_t voice) {
    if (voice != kYiVoiceEnglish) {
        voice = kYiVoiceVietnamese;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_i32(nvs, NVS_KEY_YI_VOICE, static_cast<int32_t>(voice));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

/** Pick one MAC from the active Young Innovators voice pool into custom_mac NVS. */
inline const char* PickAndSaveYoungInnovatorsMac() {
    if (ReadYiVoiceFromNvs() == kYiVoiceEnglish) {
        return PickAndSaveFromPool(kYiEnglishMacPool, kYiEnglishMacPoolCount);
    }
    return PickAndSaveFromPool(kYiVietnameseMacPool, kYiVietnameseMacPoolCount);
}

inline uint8_t ReadFlVoiceFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return kFlVoiceVietnamese;
    }
    int32_t voice = kFlVoiceVietnamese;
    nvs_get_i32(nvs, NVS_KEY_FL_VOICE, &voice);
    nvs_close(nvs);
    if (voice != kFlVoiceEnglish) {
        return kFlVoiceVietnamese;
    }
    return kFlVoiceEnglish;
}

inline bool WriteFlVoiceToNvs(uint8_t voice) {
    if (voice != kFlVoiceEnglish) {
        voice = kFlVoiceVietnamese;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_i32(nvs, NVS_KEY_FL_VOICE, static_cast<int32_t>(voice));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

/** Pick one MAC from the active Future Leaders voice pool into custom_mac NVS. */
inline const char* PickAndSaveFutureLeadersMac() {
    if (ReadFlVoiceFromNvs() == kFlVoiceEnglish) {
        return PickAndSaveFromPool(kFlEnglishMacPool, kFlEnglishMacPoolCount);
    }
    return PickAndSaveFromPool(kFlVietnameseMacPool, kFlVietnameseMacPoolCount);
}

inline uint8_t ReadIeltsVoiceFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return kIeltsVoiceVietnamese;
    }
    int32_t voice = kIeltsVoiceVietnamese;
    nvs_get_i32(nvs, NVS_KEY_IELTS_VOICE, &voice);
    nvs_close(nvs);
    if (voice != kIeltsVoiceEnglish) {
        return kIeltsVoiceVietnamese;
    }
    return kIeltsVoiceEnglish;
}

inline bool WriteIeltsVoiceToNvs(uint8_t voice) {
    if (voice != kIeltsVoiceEnglish) {
        voice = kIeltsVoiceVietnamese;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_i32(nvs, NVS_KEY_IELTS_VOICE, static_cast<int32_t>(voice));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

inline const char* PickAndSaveIeltsMac() {
    if (ReadIeltsVoiceFromNvs() == kIeltsVoiceEnglish) {
        return PickAndSaveFromPool(kIeltsEnglishMacPool, kIeltsEnglishMacPoolCount);
    }
    return PickAndSaveFromPool(kIeltsVietnameseMacPool, kIeltsVietnameseMacPoolCount);
}

inline uint8_t ReadToeicVoiceFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) != ESP_OK) {
        return kToeicVoiceVietnamese;
    }
    int32_t voice = kToeicVoiceVietnamese;
    nvs_get_i32(nvs, NVS_KEY_TOEIC_VOICE, &voice);
    nvs_close(nvs);
    if (voice != kToeicVoiceEnglish) {
        return kToeicVoiceVietnamese;
    }
    return kToeicVoiceEnglish;
}

inline bool WriteToeicVoiceToNvs(uint8_t voice) {
    if (voice != kToeicVoiceEnglish) {
        voice = kToeicVoiceVietnamese;
    }
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_i32(nvs, NVS_KEY_TOEIC_VOICE, static_cast<int32_t>(voice));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

inline const char* PickAndSaveToeicMac() {
    if (ReadToeicVoiceFromNvs() == kToeicVoiceEnglish) {
        return PickAndSaveFromPool(kToeicEnglishMacPool, kToeicEnglishMacPoolCount);
    }
    return PickAndSaveFromPool(kToeicVietnameseMacPool, kToeicVietnameseMacPoolCount);
}

/** Pick one MAC from the daily pool and store it in custom_mac NVS. */
inline const char* PickAndSaveDailyChatMac() {
    return PickAndSaveFromPool(kDailyChatMacPool, kDailyChatMacPoolCount);
}

/** Pick+save pool MAC for courses that use a pool; nullptr if course has no pool. */
inline const char* PickAndSaveMacPoolForCourse(uint8_t idx) {
    if (idx == kDefaultPresetMacIndex) {
        return PickAndSaveExplorersMac();
    }
    if (idx == kYoungInnovatorsPresetMacIndex) {
        return PickAndSaveYoungInnovatorsMac();
    }
    if (idx == kFutureLeadersPresetMacIndex) {
        return PickAndSaveFutureLeadersMac();
    }
    if (idx == kIeltsPresetMacIndex) {
        return PickAndSaveIeltsMac();
    }
    if (idx == kToeicPresetMacIndex) {
        return PickAndSaveToeicMac();
    }
    if (idx == kDailyChatPresetMacIndex) {
        return PickAndSaveDailyChatMac();
    }
    return nullptr;
}

/**
 * Call once early after NVS init (each boot).
 * Explorers / daily-chat: randomize Device-Id from their MAC pool into custom_mac.
 */
inline void ApplyMacPoolIdentityOnBoot() {
    const uint8_t idx = ReadPresetMacIndexFromNvs();
    if (!CourseUsesMacPool(idx)) {
        return;
    }
    (void)PickAndSaveMacPoolForCourse(idx);
}

/** @deprecated use ApplyMacPoolIdentityOnBoot */
inline void ApplyDailyChatIdentityOnBoot() {
    ApplyMacPoolIdentityOnBoot();
}

#endif
