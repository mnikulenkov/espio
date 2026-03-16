/**
 * @file err.h
 * @brief Unified error handling types and utilities.
 * 
 * This header provides a unified error handling system across all components.
 * Errors are identified by a combination of component ID and error code,
 * with optional rich context stored in thread-local storage.
 * 
 * @example
 * // Component implementation
 * int32_t espio_audio_create(const espio_audio_config_t* config) {
 *     if (!config) {
 *         ESPIO_AUDIO_RETURN(ESPIO_INVALID_ARG, 0, "null config");
 *     }
 *     ESPIO_AUDIO_RETURN_OK();
 * }
 * 
 * // Caller
 * int32_t result = espio_audio_create(&config);
 * if (espio_err_is_fail(result)) {
 *     const espio_err_context_t* ctx = espio_err_get_last();
 *     ESPIO_LOGE(TAG, "Failed: %s", ctx->message);
 * }
 */

#ifndef ESPIO_ERR_H
#define ESPIO_ERR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Component Identifiers
 *============================================================================*/

/**
 * @brief Component identifiers for error source tracking.
 */
typedef enum {
    ESPIO_COMPONENT_NONE = 0,      ///< No component / unset
    ESPIO_COMPONENT_COMMON = 1,    ///< Common/shared error
    ESPIO_COMPONENT_AUDIO = 2,     ///< Audio component
    ESPIO_COMPONENT_BUTTON = 3,    ///< Button component
    ESPIO_COMPONENT_DISPLAY = 4,   ///< Display component
    ESPIO_COMPONENT_SD = 5,        ///< SD card component
    // Future components can be added here
} espio_component_t;

/*============================================================================
 * Common Error Codes - Shared semantic baseline
 * 
 * These codes provide a shared vocabulary for common failure modes.
 * Components should use these when appropriate, but can define their
 * own specific codes when more precision is needed.
 *============================================================================*/

/**
 * @brief Common error codes shared across all components.
 */
typedef enum {
    // Success
    ESPIO_OK = 0,                      ///< Operation completed successfully
    
    // Common errors (1-99)
    ESPIO_INVALID_ARG = 1,             ///< Invalid argument provided
    ESPIO_NO_MEM = 2,                  ///< Memory allocation failed
    ESPIO_TIMEOUT = 3,                 ///< Operation timed out
    ESPIO_BUSY = 4,                    ///< Resource is busy
    ESPIO_NOT_FOUND = 5,               ///< Requested resource not found
    ESPIO_IO = 6,                      ///< Generic I/O failure
    ESPIO_UNSUPPORTED = 7,             ///< Feature not supported
    ESPIO_INTERNAL = 8,                ///< Internal consistency failure
    ESPIO_NOT_INITIALIZED = 9,         ///< Component not initialized
    ESPIO_ALREADY_INITIALIZED = 10,    ///< Component already initialized
    ESPIO_PERMISSION = 11,             ///< Permission denied
    ESPIO_OVERFLOW = 12,               ///< Buffer or queue overflow
    ESPIO_UNDERFLOW = 13,              ///< Buffer or queue underflow
    ESPIO_CANCELLED = 14,              ///< Operation was cancelled
    ESPIO_NOT_READY = 15,              ///< Resource not ready
} espio_err_t;

/*============================================================================
 * Error Context Structure
 *============================================================================*/

/**
 * @brief Rich error context for detailed error reporting.
 * 
 * This structure holds all information about an error, including
 * the error code, source component, native error code (if wrapping
 * another error system), a human-readable message, and source location.
 */
typedef struct {
    int32_t code;                    ///< Unified error code
    espio_component_t component;     ///< Component that generated the error
    int32_t native_code;             ///< Original error (esp_err_t, errno, etc.)
    const char* message;             ///< Static error message (may be NULL)
    const char* file;                ///< Source file (for debugging, may be NULL)
    int line;                        ///< Source line (for debugging)
} espio_err_context_t;

/*============================================================================
 * Core Error Functions
 *============================================================================*/

/**
 * @brief Check if an error code indicates success.
 * 
 * @param code Error code to check
 * @return true if the code indicates success (ERR_OK)
 */
static inline bool espio_err_is_ok(int32_t code) {
    return code == ESPIO_OK;
}

/**
 * @brief Check if an error code indicates failure.
 * 
 * @param code Error code to check
 * @return true if the code indicates failure (non-zero)
 */
static inline bool espio_err_is_fail(int32_t code) {
    return code != ESPIO_OK;
}

/**
 * @brief Get human-readable name for a common error code.
 * 
 * @param code Error code
 * @return Static string name, or "UNKNOWN" if not found
 */
const char* espio_err_common_name(int32_t code);

/**
 * @brief Get human-readable description for a common error code.
 * 
 * @param code Error code
 * @return Static string description, or NULL if not found
 */
const char* espio_err_common_desc(int32_t code);

/**
 * @brief Get the component name from a component ID.
 * 
 * @param component Component identifier
 * @return Static string name
 */
const char* espio_err_component_name(espio_component_t component);

/*============================================================================
 * Thread-Local Last Error Storage
 *============================================================================*/

/**
 * @brief Set the last error context for the current thread.
 * 
 * This is typically called internally by the ERR_RETURN macros.
 * Applications can retrieve the full context using err_get_last().
 * 
 * @param ctx Error context to store (copied internally)
 */
void espio_err_set_last(const espio_err_context_t* ctx);

/**
 * @brief Get the last error context for the current thread.
 * 
 * Returns the context stored by the most recent err_set_last() call.
 * If no error was set, returns a context with ERR_OK.
 * 
 * @return Pointer to thread-local error context (valid until next err_set_last)
 * @note The returned pointer is valid only until the next call to err_set_last()
 *       or err_clear_last() in the same thread.
 */
const espio_err_context_t* espio_err_get_last(void);

/**
 * @brief Clear the last error for the current thread.
 * 
 * Resets the thread-local error context to ERR_OK.
 */
void espio_err_clear_last(void);

/**
 * @brief Get and clear the last error in one operation.
 * 
 * Returns a copy of the current error context and resets it to ERR_OK.
 * 
 * @return Copy of the error context before clearing
 */
espio_err_context_t espio_err_take_last(void);

/*============================================================================
 * Error Context Construction Helpers
 *============================================================================*/

/**
 * @brief Internal helper to build error context.
 *
 * @param code Error code
 * @param comp Component identifier
 * @param native Native error code
 * @param msg Error message
 * @param file Source file
 * @param line Source line
 * @return Initialized error context
 */
static inline espio_err_context_t espio_err_make_context_full(
    int32_t code,
    espio_component_t comp,
    int32_t native,
    const char* msg,
    const char* file,
    int line
) {
    espio_err_context_t ctx;
    ctx.code = code;
    ctx.component = comp;
    ctx.native_code = native;
    ctx.message = msg;
    ctx.file = file;
    ctx.line = line;
    return ctx;
}

/**
 * @brief Create an error context from the current source location.
 */
#define ESPIO_ERR_CONTEXT_HERE(code, comp, native, msg) \
    espio_err_make_context_full((code), (comp), (native), (msg), __FILE__, __LINE__)

/**
 * @brief Create an error context (simplified, without file/line).
 */
#define ESPIO_ERR_CONTEXT(code, comp, native, msg) \
    espio_err_make_context_full((code), (comp), (native), (msg), NULL, 0)

/**
 * @brief Create a simple success context.
 */
#define ESPIO_ERR_CONTEXT_OK(comp) \
    ESPIO_ERR_CONTEXT(ESPIO_OK, comp, 0, NULL)

/*============================================================================
 * Convenience Macros for Setting/Returning Errors
 *
 * These macros combine setting thread-local context and returning error codes.
 * Functions return simple int32_t error codes; full context is available via
 * err_get_last().
 *============================================================================*/

/**
 * @brief Internal helper to set error and return.
 */
static inline int32_t espio_err_return_impl(
    int32_t code,
    espio_component_t comp,
    int32_t native,
    const char* msg,
    const char* file,
    int line
) {
    espio_err_context_t ctx = espio_err_make_context_full(code, comp, native, msg, file, line);
    espio_err_set_last(&ctx);
    return code;
}

/**
 * @brief Set thread-local error context and return the error code.
 *
 * This macro sets the thread-local error context with full source location
 * information and returns the error code.
 *
 * @param code Error code to return
 * @param comp Component identifier
 * @param native Native error code (e.g., esp_err_t, errno, or 0)
 * @param msg Human-readable error message (static string)
 * @return The error code (int32_t)
 */
#define ESPIO_ERR_RETURN(code, comp, native, msg) \
    return espio_err_return_impl((code), (comp), (native), (msg), __FILE__, __LINE__)

/**
 * @brief Return success (clear error context and return ERR_OK).
 *
 * @param comp Component identifier
 * @return ERR_OK (0)
 */
#define ESPIO_ERR_RETURN_OK(comp) \
    do { \
        espio_err_clear_last(); \
        return ESPIO_OK; \
    } while(0)

/**
 * @brief Set error and return if condition is true.
 *
 * @param cond Condition to check
 * @param code Error code to return if condition is true
 * @param comp Component identifier
 * @param native Native error code
 * @param msg Human-readable error message
 */
#define ESPIO_ERR_RETURN_IF(cond, code, comp, native, msg) \
    do { if (cond) ESPIO_ERR_RETURN(code, comp, native, msg); } while(0)

/**
 * @brief Internal helper to set error without returning.
 */
static inline void espio_err_set_impl(
    int32_t code,
    espio_component_t comp,
    int32_t native,
    const char* msg,
    const char* file,
    int line
) {
    espio_err_context_t ctx = espio_err_make_context_full(code, comp, native, msg, file, line);
    espio_err_set_last(&ctx);
}

/**
 * @brief Set error context without returning (for internal use).
 *
 * Use this when you need to set the error context but not immediately return,
 * such as in cleanup code or when propagating errors through multiple layers.
 *
 * @param code Error code
 * @param comp Component identifier
 * @param native Native error code
 * @param msg Human-readable error message
 */
#define ESPIO_ERR_SET(code, comp, native, msg) \
    espio_err_set_impl((code), (comp), (native), (msg), __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif // ESPIO_ERR_H
