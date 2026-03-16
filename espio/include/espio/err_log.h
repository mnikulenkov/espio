/**
 * @file err_log.h
 * @brief Error logging integration with the unified error system.
 * 
 * This header provides macros for logging error contexts at various
 * severity levels. It integrates with the existing log.h system and
 * respects its compile-time configuration.
 * 
 * @example
 * int32_t result = espio_audio_create(&config);
 * if (espio_err_is_fail(result)) {
 *     const espio_err_context_t* ctx = espio_err_get_last();
 *     ESPIO_ERR_LOGE(TAG, *ctx);
 * }
 */

#ifndef ESPIO_ERR_LOG_H
#define ESPIO_ERR_LOG_H

#include "espio/err.h"
#include "espio/log.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Error Context Logging Macros
 *============================================================================*/

/**
 * @brief Log an error context at ERROR level.
 * 
 * Logs the component, error code name, message, and native error code.
 * Only logs if the error code indicates failure.
 * 
 * @param tag Log tag
 * @param ctx Error context to log
 */
#define ESPIO_ERR_LOGE(tag, ctx) do { \
    if (espio_err_is_fail((ctx).code)) { \
        ESPIO_LOGE(tag, "[%s] %s: %s (native=%d)", \
            espio_err_component_name((ctx).component), \
            espio_err_common_name((ctx).code), \
            (ctx).message ? (ctx).message : "", \
            (ctx).native_code); \
    } \
} while(0)

/**
 * @brief Log an error context at WARN level.
 * 
 * @param tag Log tag
 * @param ctx Error context to log
 */
#define ESPIO_ERR_LOGW(tag, ctx) do { \
    if (espio_err_is_fail((ctx).code)) { \
        ESPIO_LOGW(tag, "[%s] %s: %s (native=%d)", \
            espio_err_component_name((ctx).component), \
            espio_err_common_name((ctx).code), \
            (ctx).message ? (ctx).message : "", \
            (ctx).native_code); \
    } \
} while(0)

/**
 * @brief Log an error context at INFO level.
 * 
 * @param tag Log tag
 * @param ctx Error context to log
 */
#define ESPIO_ERR_LOGI(tag, ctx) do { \
    ESPIO_LOGI(tag, "[%s] %s", \
        espio_err_component_name((ctx).component), \
        espio_err_common_name((ctx).code)); \
} while(0)

/**
 * @brief Log an error context at DEBUG level with full details.
 * 
 * Includes file and line information for debugging.
 * 
 * @param tag Log tag
 * @param ctx Error context to log
 */
#define ESPIO_ERR_LOGD(tag, ctx) do { \
    ESPIO_LOGD(tag, "[%s] %s: %s (native=%d, file=%s:%d)", \
        espio_err_component_name((ctx).component), \
        espio_err_common_name((ctx).code), \
        (ctx).message ? (ctx).message : "", \
        (ctx).native_code, \
        (ctx).file ? (ctx).file : "", \
        (ctx).line); \
} while(0)

/*============================================================================
 * Convenience Macros for Common Patterns
 *============================================================================*/

/**
 * @brief Log the last error and return it.
 * 
 * Retrieves the last error from thread-local storage, logs it at ERROR level,
 * and returns the error code.
 * 
 * @param tag Log tag
 * @return The error code from espio_err_get_last()
 */
#define ESPIO_ERR_LOG_LAST_AND_RETURN(tag) do { \
    const espio_err_context_t* _ctx = espio_err_get_last(); \
    ESPIO_ERR_LOGE(tag, *_ctx); \
    return _ctx->code; \
} while(0)

/**
 * @brief Check result and log on failure.
 * 
 * If the result indicates failure, retrieves and logs the last error.
 * 
 * @param tag Log tag
 * @param result Error code to check
 */
#define ESPIO_ERR_LOG_ON_FAIL(tag, result) do { \
    if (espio_err_is_fail(result)) { \
        const espio_err_context_t* _ctx = espio_err_get_last(); \
        ESPIO_ERR_LOGE(tag, *_ctx); \
    } \
} while(0)

#ifdef __cplusplus
}
#endif

#endif // ESPIO_ERR_LOG_H
