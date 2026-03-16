/**
 * @file err_sd.h
 * @brief SD card component specific error codes.
 * 
 * This header defines error codes specific to the SD card component.
 * Error codes use a two-level identification system:
 * - The component field (ERR_COMPONENT_SD) identifies the source
 * - The code field identifies the specific error
 * 
 * Error code ranges:
 * - 0: Success
 * - 1-99: Reserved for common errors (use ERR_* from err.h)
 * - 100-999: SD component errors
 * - 1000+: Driver/host-specific errors
 */

#ifndef ESPIO_SD_ERR_H
#define ESPIO_SD_ERR_H

#include "espio/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * SD Error Codes
 *============================================================================*/

/**
 * @brief SD-specific error codes.
 * 
 * Error codes start from 100 to leave room for common errors (0-99).
 * The component field in err_context_t identifies this as an SD error.
 */
typedef enum {
    // Success
    ESPIO_SD_OK = 0,
    
    // SD-specific errors (start from 100)
    ESPIO_SD_ERR_ALREADY_MOUNTED = 100,     ///< Card already mounted
    ESPIO_SD_ERR_NOT_MOUNTED = 101,         ///< Card not mounted
    ESPIO_SD_ERR_ALREADY_EXISTS = 102,      ///< File/directory already exists
    ESPIO_SD_ERR_NO_SPACE = 103,            ///< Storage full
    ESPIO_SD_ERR_TOO_MANY_OPEN_FILES = 104, ///< Open file limit reached
    ESPIO_SD_ERR_MOUNT_FAILED = 105,        ///< Mount operation failed
    ESPIO_SD_ERR_UNMOUNT_FAILED = 106,      ///< Unmount operation failed
    ESPIO_SD_ERR_CARD_INIT_FAILED = 107,    ///< Card initialization failed
    ESPIO_SD_ERR_HOST_INIT_FAILED = 108,    ///< Host initialization failed
    ESPIO_SD_ERR_PATH_INVALID = 109,        ///< Invalid path format
    ESPIO_SD_ERR_PATH_TOO_LONG = 110,       ///< Path exceeds limit
    ESPIO_SD_ERR_POWER_FAILED = 111,        ///< Power control failed
    ESPIO_SD_ERR_FILE_NOT_OPEN = 112,       ///< File handle not open
    ESPIO_SD_ERR_FILE_CLOSED = 113,         ///< File already closed
    ESPIO_SD_ERR_READ_FAILED = 114,         ///< Read operation failed
    ESPIO_SD_ERR_WRITE_FAILED = 115,        ///< Write operation failed
    ESPIO_SD_ERR_SEEK_FAILED = 116,         ///< Seek operation failed
    ESPIO_SD_ERR_FLUSH_FAILED = 117,        ///< Flush operation failed
    ESPIO_SD_ERR_STAT_FAILED = 118,         ///< Stat operation failed
    ESPIO_SD_ERR_DELETE_FAILED = 119,       ///< Delete operation failed
    ESPIO_SD_ERR_MKDIR_FAILED = 120,        ///< Directory creation failed
    ESPIO_SD_ERR_RMDIR_FAILED = 121,        ///< Directory removal failed
    ESPIO_SD_ERR_OPENDIR_FAILED = 122,      ///< Open directory failed
    ESPIO_SD_ERR_NOT_A_DIRECTORY = 123,     ///< Path is not a directory
    ESPIO_SD_ERR_UNMOUNT_PENDING = 124,     ///< Unmount pending, operations blocked
    
    // Driver/host-specific errors (drivers can add codes starting from 1000)
    ESPIO_SD_ERR_DRIVER_BASE = 1000,        ///< Base for driver-specific errors
} espio_sd_err_t;

/*============================================================================
 * Helper Functions
 *============================================================================*/

/**
 * @brief Check if SD error code indicates success.
 * 
 * @param code Error code to check
 * @return true if the code indicates success (SD_OK)
 */
static inline bool espio_sd_is_ok(int32_t code) {
    return code == ESPIO_SD_OK;
}

/**
 * @brief Get human-readable name for an SD error code.
 * 
 * @param code Error code
 * @return Static string name, or NULL if not an SD-specific error
 */
const char* espio_sd_err_name(int32_t code);

/**
 * @brief Get human-readable description for an SD error code.
 * 
 * @param code Error code
 * @return Static string description, or NULL if not an SD-specific error
 */
const char* espio_sd_err_desc(int32_t code);

/*============================================================================
 * Error Context Macros
 *============================================================================*/

/**
 * @brief Create SD error context.
 */
#define ESPIO_SD_ERR_CONTEXT(code, native, msg) \
    ESPIO_ERR_CONTEXT(code, ESPIO_COMPONENT_SD, native, msg)

#define ESPIO_SD_ERR_CONTEXT_HERE(code, native, msg) \
    ESPIO_ERR_CONTEXT_HERE(code, ESPIO_COMPONENT_SD, native, msg)

/*============================================================================
 * Return Macros
 *============================================================================*/

/**
 * @brief Return SD error with thread-local context.
 * 
 * Sets the thread-local error context and returns the error code.
 * 
 * @param code Error code (use `ESPIO_SD_*` or `ESPIO_*` constants)
 * @param native Native error code (e.g., esp_err_t, errno, or 0)
 * @param msg Human-readable error message (static string)
 */
#define ESPIO_SD_RETURN(code, native, msg) \
    ESPIO_ERR_RETURN(code, ESPIO_COMPONENT_SD, native, msg)

/**
 * @brief Return SD success.
 */
#define ESPIO_SD_RETURN_OK() \
    ESPIO_ERR_RETURN_OK(ESPIO_COMPONENT_SD)

/**
 * @brief Return SD error if condition is true.
 */
#define ESPIO_SD_RETURN_IF(cond, code, native, msg) \
    ESPIO_ERR_RETURN_IF(cond, code, ESPIO_COMPONENT_SD, native, msg)

/**
 * @brief Set SD error context without returning.
 */
#define ESPIO_SD_SET_ERROR(code, native, msg) \
    ESPIO_ERR_SET(code, ESPIO_COMPONENT_SD, native, msg)

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_SD_ERR_H */
