#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <cstdint>
#include <string>

/*
 * Keeps device logs readable without a USB cable (Self-Control page, tab Log).
 *
 * - Every ESP_LOG line is copied into a PSRAM ring (live log).
 * - The last few KB are mirrored into RTC memory, and the panic handler output
 *   (reason, registers, backtrace) is captured there too. RTC memory survives
 *   panic / watchdog / software resets, so the next boot can show why the
 *   device rebooted. A power-on or brownout reset clears it.
 */
namespace LogBuffer {

/** Call first thing in app_main. */
void Init();

/**
 * Log text written after sequence number `from` (bytes since boot).
 * If `from` is older than the ring, returns everything still kept.
 * *next receives the sequence number to pass next time.
 */
std::string Read(uint64_t from, uint64_t* next);

/** Reset reason of this boot, crash output and log tail of the previous boot. */
std::string PreviousBootReport();

}  // namespace LogBuffer

#endif  // LOG_BUFFER_H
