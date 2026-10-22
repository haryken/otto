#include "log_buffer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_private/panic_internal.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr size_t kRingSize = 64 * 1024;
constexpr size_t kTailSize = 4096;
constexpr size_t kCrashSize = 3072;
constexpr uint32_t kMagic = 0x4C4F4731;  // "LOG1"

struct RtcLog {
    uint32_t magic;
    uint32_t tail_pos;   // bytes written to tail since boot
    uint32_t crash_len;
    char tail[kTailSize];
    char crash[kCrashSize];
};

// Not cleared by panic / watchdog / software resets.
RTC_NOINIT_ATTR RtcLog s_rtc;

char* s_ring = nullptr;
uint64_t s_total = 0;
bool s_in_escape = false;
char s_line[512];
SemaphoreHandle_t s_mutex = nullptr;
vprintf_like_t s_prev_vprintf = nullptr;
std::string s_previous;

const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON (bật nguồn)";
        case ESP_RST_EXT: return "EXT (chân reset ngoài)";
        case ESP_RST_SW: return "SW (esp_restart: reboot chủ động, OTA, đổi chế độ)";
        case ESP_RST_PANIC: return "PANIC (crash)";
        case ESP_RST_INT_WDT: return "INT_WDT (treo ngắt)";
        case ESP_RST_TASK_WDT: return "TASK_WDT (task bị treo)";
        case ESP_RST_WDT: return "WDT (watchdog khác)";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT (sụt áp: pin/nguồn yếu)";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH (nhiễu nguồn)";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default: return "UNKNOWN";
    }
}

// Caller holds s_mutex. Drops ANSI color sequences.
void Append(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const char c = data[i];
        if (s_in_escape) {
            if (c == 'm') {
                s_in_escape = false;
            }
            continue;
        }
        if (c == '\x1b') {
            s_in_escape = true;
            continue;
        }
        if (s_ring != nullptr) {
            s_ring[s_total % kRingSize] = c;
        }
        s_total++;
        s_rtc.tail[s_rtc.tail_pos % kTailSize] = c;
        s_rtc.tail_pos++;
    }
}

int LogVprintf(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    const int ret = s_prev_vprintf != nullptr ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const int n = vsnprintf(s_line, sizeof(s_line), fmt, copy);
        if (n > 0) {
            Append(s_line, std::min<size_t>(n, sizeof(s_line) - 1));
            if (static_cast<size_t>(n) >= sizeof(s_line)) {
                Append("...\n", 4);
            }
        }
        xSemaphoreGive(s_mutex);
    }
    va_end(copy);
    return ret;
}

std::string RtcTail() {
    const uint32_t pos = s_rtc.tail_pos;
    if (pos <= kTailSize) {
        return std::string(s_rtc.tail, pos);
    }
    const size_t start = pos % kTailSize;
    std::string out(s_rtc.tail + start, kTailSize - start);
    out.append(s_rtc.tail, start);
    return out;
}

void IRAM_ATTR CrashPutc(char c) {
    if (s_rtc.crash_len < kCrashSize) {
        s_rtc.crash[s_rtc.crash_len++] = c;
    }
}

void IRAM_ATTR CrashPuts(const char* s) {
    while (s != nullptr && *s != '\0') {
        CrashPutc(*s++);
    }
}

void IRAM_ATTR CrashHex(int h) {
    for (int i = 0; i < 8; i++) {
        const int d = (h >> 28) & 0xf;
        CrashPutc(d < 10 ? '0' + d : 'a' + d - 10);
        h <<= 4;
    }
}

void IRAM_ATTR CrashDec(int d) {
    if (d >= 10) {
        CrashPutc('0' + (d / 10) % 10);
    }
    CrashPutc('0' + d % 10);
}

}  // namespace

// Panic output goes straight to the UART; these --wrap hooks (main/CMakeLists.txt)
// copy it into RTC memory as well. Calls inside panic.c itself are not wrapped,
// so the "Guru Meditation" header is rebuilt from panic_info_t.
extern "C" {
void __real_esp_panic_handler(panic_info_t* info);
void __real_panic_print_char(char c);
void __real_panic_print_str(const char* str);
void __real_panic_print_hex(int h);
void __real_panic_print_dec(int d);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
    s_rtc.magic = kMagic;
    CrashPuts("Guru Meditation Error: Core ");
    CrashDec(info->core);
    if (g_panic_abort) {
        CrashPuts(" abort: ");
        CrashPuts(g_panic_abort_details);
    } else {
        CrashPuts(" panic'ed (");
        CrashPuts(info->reason);
        CrashPuts("). ");
        CrashPuts(info->description);
    }
    CrashPuts("\r\n");
    __real_esp_panic_handler(info);
}

void IRAM_ATTR __wrap_panic_print_char(char c) {
    CrashPutc(c);
    __real_panic_print_char(c);
}

void IRAM_ATTR __wrap_panic_print_str(const char* str) {
    CrashPuts(str);
    __real_panic_print_str(str);
}

void IRAM_ATTR __wrap_panic_print_hex(int h) {
    CrashHex(h);
    __real_panic_print_hex(h);
}

void IRAM_ATTR __wrap_panic_print_dec(int d) {
    CrashDec(d);
    __real_panic_print_dec(d);
}
}

namespace LogBuffer {

void Init() {
    const esp_reset_reason_t reason = esp_reset_reason();
    s_previous = std::string("Lý do khởi động lần này: ") + ResetReasonName(reason) + "\n";
    if (reason != ESP_RST_POWERON && s_rtc.magic == kMagic && s_rtc.crash_len <= kCrashSize) {
        if (s_rtc.crash_len > 0) {
            s_previous += "\n----- Crash (lần chạy trước) -----\n";
            s_previous.append(s_rtc.crash, s_rtc.crash_len);
            s_previous += "\n";
        }
        s_previous += "\n----- Log cuối của lần chạy trước -----\n";
        s_previous += RtcTail();
    } else if (reason != ESP_RST_POWERON) {
        s_previous += "(Không có log lần chạy trước)\n";
    }

    s_rtc.tail_pos = 0;
    s_rtc.crash_len = 0;
    s_rtc.magic = kMagic;

    s_ring = static_cast<char*>(heap_caps_malloc(kRingSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_mutex = xSemaphoreCreateMutex();
    s_prev_vprintf = esp_log_set_vprintf(LogVprintf);
}

std::string Read(uint64_t from, uint64_t* next) {
    std::string out;
    if (s_mutex == nullptr || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        *next = from;
        return out;
    }
    const uint64_t total = s_total;
    const uint64_t oldest = s_ring != nullptr && total > kRingSize ? total - kRingSize : 0;
    uint64_t start = from;
    if (start > total || start < oldest) {
        start = oldest;
    }
    if (s_ring != nullptr && total > start) {
        out.reserve(total - start);
        for (uint64_t i = start; i < total;) {
            const size_t idx = i % kRingSize;
            const size_t n = std::min<uint64_t>(total - i, kRingSize - idx);
            out.append(s_ring + idx, n);
            i += n;
        }
    }
    xSemaphoreGive(s_mutex);
    *next = total;
    return out;
}

std::string PreviousBootReport() {
    return s_previous;
}

}  // namespace LogBuffer
