/**
 * @file log.h
 * @brief Common Logging Configuration
 *
 * This header provides compile-time configurable logging macros that can be
 * used across all components. Control logging behavior by defining these
 * before including this header:
 *
 * - ESPIO_LOG_ENABLED:    Enable/disable all logging (default: 1)
 * - ESPIO_LOG_LEVEL:      Minimum log level 0-5 (default: 3 = INFO)
 *                          0=NONE, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG, 5=VERBOSE
 *
 * Or define via compiler flags: -DESPIO_LOG_ENABLED=0 -DESPIO_LOG_LEVEL=2
 *
 * @example
 * // Disable logging completely
 * #define ESPIO_LOG_ENABLED 0
 *
 * // Set to warnings only
 * #define ESPIO_LOG_LEVEL 2
 *
 * // Usage in code
 * #include "log.h"
 *
 * static const char* TAG = "my_module";
 *
 * ESPIO_LOGE(TAG, "Error occurred: %d", error_code);
 * ESPIO_LOGI(TAG, "Initialized successfully");
 * ESPIO_LOGD(TAG, "Debug value: 0x%08X", value);
 */

#ifndef ESPIO_LOG_H
#define ESPIO_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration Defaults
 *============================================================================*/

#ifndef ESPIO_LOG_ENABLED
#define ESPIO_LOG_ENABLED 1   ///< Enable logging (set to 0 to disable all)
#endif

#ifndef ESPIO_LOG_LEVEL
#define ESPIO_LOG_LEVEL 3     ///< Default log level: 0=NONE, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG, 5=VERBOSE
#endif

/*============================================================================
 * Log Level Definitions
 *============================================================================*/

#define ESPIO_LOG_LEVEL_NONE     0
#define ESPIO_LOG_LEVEL_ERROR    1
#define ESPIO_LOG_LEVEL_WARN     2
#define ESPIO_LOG_LEVEL_INFO     3
#define ESPIO_LOG_LEVEL_DEBUG    4
#define ESPIO_LOG_LEVEL_VERBOSE  5

/*============================================================================
 * Logging Implementation
 *============================================================================*/

#if ESPIO_LOG_ENABLED

    /* Try to use ESP-IDF logging if available */
    #if defined(__has_include)
        #if __has_include("esp_log.h")
            #include "esp_log.h"
            #define ESPIO_USE_ESP_LOG 1
        #endif
    #endif

    /* If ESP-IDF logging is available, use it with level filtering */
    #ifdef ESPIO_USE_ESP_LOG

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_ERROR
            #define ESPIO_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGE(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_WARN
            #define ESPIO_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGW(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_INFO
            #define ESPIO_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGI(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_DEBUG
            #define ESPIO_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGD(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_VERBOSE
            #define ESPIO_LOGV(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGV(tag, fmt, ...)
        #endif

    #else
        /*====================================================================
         * Fallback: Simple printf-based logging for non-ESP-IDF platforms
         *====================================================================*/
        #include <stdio.h>

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_ERROR
            #define ESPIO_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGE(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_WARN
            #define ESPIO_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGW(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_INFO
            #define ESPIO_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGI(tag, fmt, ...)
        #endif

        #if ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_DEBUG
            #define ESPIO_LOGD(tag, fmt, ...) printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
        #else
            #define ESPIO_LOGD(tag, fmt, ...)
        #endif

        #define ESPIO_LOGV(tag, fmt, ...)

    #endif /* ESPIO_USE_ESP_LOG */

#else
    /*========================================================================
     * Logging Disabled - All macros expand to nothing
     *========================================================================*/
    #define ESPIO_LOGE(tag, fmt, ...)
    #define ESPIO_LOGW(tag, fmt, ...)
    #define ESPIO_LOGI(tag, fmt, ...)
    #define ESPIO_LOGD(tag, fmt, ...)
    #define ESPIO_LOGV(tag, fmt, ...)

#endif /* ESPIO_LOG_ENABLED */

/*============================================================================
 * Helper Macros
 *============================================================================*/

/**
 * @brief Use to wrap debug-only code blocks
 *
 * @example
 * ESPIO_DEBUG_CODE(
 *     verbose_debug_print(state);
 * )
 */
#if ESPIO_LOG_ENABLED && ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_DEBUG
    #define ESPIO_DEBUG_CODE(code) code
#else
    #define ESPIO_DEBUG_CODE(code)
#endif

/**
 * @brief Check if debug logging is enabled at compile time
 */
#define ESPIO_DEBUG_ENABLED (ESPIO_LOG_ENABLED && ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_DEBUG)

/**
 * @brief Log hex dump of data (debug level only)
 */
#if ESPIO_LOG_ENABLED && ESPIO_LOG_LEVEL >= ESPIO_LOG_LEVEL_DEBUG
    #include <string.h>
    static inline void espio_log_hexdump(const char* tag, const char* title, const void* data, size_t len) {
        const uint8_t* bytes = (const uint8_t*)data;
        ESPIO_LOGD(tag, "%s (%zu bytes):", title, len);
        for (size_t i = 0; i < len && i < 64; i++) {
            if (i % 16 == 0) printf("  ");
            printf("%02X ", bytes[i]);
            if ((i + 1) % 16 == 0) printf("\n");
        }
        if (len % 16 != 0) printf("\n");
    }
    #define ESPIO_HEXDUMP(tag, title, data, len) espio_log_hexdump(tag, title, data, len)
#else
    #define ESPIO_HEXDUMP(tag, title, data, len)
#endif

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_LOG_H */
