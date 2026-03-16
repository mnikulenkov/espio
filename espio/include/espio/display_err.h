/**
 * @file err_display.h
 * @brief Display component specific error codes.
 * 
 * This header defines error codes specific to the display component.
 * Error codes use a two-level identification system:
 * - The component field (ERR_COMPONENT_DISPLAY) identifies the source
 * - The code field identifies the specific error
 * 
 * Error code ranges:
 * - 0: Success
 * - 1-99: Reserved for common errors (use ERR_* from err.h)
 * - 100-999: Display component errors
 * - 1000+: Driver-specific errors
 */

#ifndef ESPIO_DISPLAY_ERR_H
#define ESPIO_DISPLAY_ERR_H

#include "espio/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Display Error Codes
 *============================================================================*/

/**
 * @brief Display-specific error codes.
 * 
 * Error codes start from 100 to leave room for common errors (0-99).
 * The component field in err_context_t identifies this as a display error.
 */
typedef enum {
    // Success
    ESPIO_DISPLAY_OK = 0,
    
    // Display-specific errors (start from 100)
    ESPIO_DISPLAY_ERR_DRIVER_FAILED = 100,     ///< Driver initialization failed
    ESPIO_DISPLAY_ERR_SPI_INIT = 101,          ///< SPI initialization failed
    ESPIO_DISPLAY_ERR_SPI_WRITE = 102,         ///< SPI write operation failed
    ESPIO_DISPLAY_ERR_PANEL_INIT = 103,        ///< Panel initialization failed
    ESPIO_DISPLAY_ERR_PANEL_RESET = 104,       ///< Panel reset failed
    ESPIO_DISPLAY_ERR_BACKLIGHT = 105,         ///< Backlight control failed
    ESPIO_DISPLAY_ERR_FRAMEBUFFER = 106,       ///< Framebuffer allocation failed
    ESPIO_DISPLAY_ERR_DMA = 107,               ///< DMA transfer failed
    ESPIO_DISPLAY_ERR_OUT_OF_BOUNDS = 108,     ///< Coordinates out of bounds
    ESPIO_DISPLAY_ERR_FLUSH_IN_PROGRESS = 109, ///< Flush already in progress
    ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER = 110,    ///< No framebuffer allocated
    ESPIO_DISPLAY_ERR_INVALID_BUFFER_MODE = 111, ///< Invalid buffer mode
    ESPIO_DISPLAY_ERR_DOUBLE_BUFFER_SWAP = 112, ///< Double buffer swap failed
    ESPIO_DISPLAY_ERR_DRIVER_NOT_SET = 113,    ///< Driver not set in configuration
    
    // Format-related errors (120-129)
    ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT = 120,  ///< Pixel format not supported
    ESPIO_DISPLAY_ERR_FORMAT_MISMATCH = 121,     ///< Data format doesn't match display
    ESPIO_DISPLAY_ERR_CONVERSION_FAILED = 122,   ///< Color format conversion failed
    
    // Partial update errors (130-139)
    ESPIO_DISPLAY_ERR_PARTIAL_NOT_SUPPORTED = 130,  ///< Partial update not supported
    ESPIO_DISPLAY_ERR_PARTIAL_INVALID_REGION = 131, ///< Invalid partial update region
    
    // Async operation errors (140-149)
    ESPIO_DISPLAY_ERR_ASYNC_BUSY = 140,      ///< Async operation already in progress
    ESPIO_DISPLAY_ERR_ASYNC_TIMEOUT = 141,   ///< Async operation timed out
    ESPIO_DISPLAY_ERR_ASYNC_CANCELLED = 142, ///< Async operation was cancelled
    
    // Continuous refresh errors (150-159)
    ESPIO_DISPLAY_ERR_CONTINUOUS_NOT_SUPPORTED = 150,  ///< Continuous refresh not supported
    ESPIO_DISPLAY_ERR_NOT_IN_CONTINUOUS_MODE = 151,    ///< Not in continuous refresh mode
    ESPIO_DISPLAY_ERR_VSYNC_TIMEOUT = 152,             ///< Vsync wait timed out
    ESPIO_DISPLAY_ERR_REFRESH_RATE = 153,              ///< Invalid refresh rate
    
    // Extended feature errors (160-169)
    ESPIO_DISPLAY_ERR_ROTATION_NOT_SUPPORTED = 160,    ///< Rotation not supported
    ESPIO_DISPLAY_ERR_POWER_STATE_NOT_SUPPORTED = 161, ///< Power state not supported
    ESPIO_DISPLAY_ERR_COMMAND_NOT_SUPPORTED = 162,     ///< Command not supported
    ESPIO_DISPLAY_ERR_COMMAND_FAILED = 163,            ///< Command execution failed
    ESPIO_DISPLAY_ERR_READ_NOT_SUPPORTED = 164,        ///< Pixel read not supported
    
    // Driver-specific errors (drivers can add codes starting from 1000)
    ESPIO_DISPLAY_ERR_DRIVER_BASE = 1000,   ///< Base for driver-specific errors
} espio_display_err_t;

/*============================================================================
 * Helper Functions
 *============================================================================*/

/**
 * @brief Check if display error code indicates success.
 * 
 * @param code Error code to check
 * @return true if the code indicates success (DISPLAY_OK)
 */
static inline bool espio_display_is_ok(int32_t code) {
    return code == ESPIO_DISPLAY_OK;
}

/**
 * @brief Get human-readable name for a display error code.
 * 
 * @param code Error code
 * @return Static string name, or NULL if not a display-specific error
 */
const char* espio_display_err_name(int32_t code);

/**
 * @brief Get human-readable description for a display error code.
 * 
 * @param code Error code
 * @return Static string description, or NULL if not a display-specific error
 */
const char* espio_display_err_desc(int32_t code);

/*============================================================================
 * Error Context Macros
 *============================================================================*/

/**
 * @brief Create display error context.
 */
#define ESPIO_DISPLAY_ERR_CONTEXT(code, native, msg) \
    ESPIO_ERR_CONTEXT(code, ESPIO_COMPONENT_DISPLAY, native, msg)

#define ESPIO_DISPLAY_ERR_CONTEXT_HERE(code, native, msg) \
    ESPIO_ERR_CONTEXT_HERE(code, ESPIO_COMPONENT_DISPLAY, native, msg)

/*============================================================================
 * Return Macros
 *============================================================================*/

/**
 * @brief Return display error with thread-local context.
 * 
 * Sets the thread-local error context and returns the error code.
 * 
 * @param code Error code (use DISPLAY_* or ERR_* constants)
 * @param native Native error code (e.g., esp_err_t, or 0)
 * @param msg Human-readable error message (static string)
 */
#define ESPIO_DISPLAY_RETURN(code, native, msg) \
    ESPIO_ERR_RETURN(code, ESPIO_COMPONENT_DISPLAY, native, msg)

/**
 * @brief Return display success.
 */
#define ESPIO_DISPLAY_RETURN_OK() \
    ESPIO_ERR_RETURN_OK(ESPIO_COMPONENT_DISPLAY)

/**
 * @brief Return display error if condition is true.
 */
#define ESPIO_DISPLAY_RETURN_IF(cond, code, native, msg) \
    ESPIO_ERR_RETURN_IF(cond, code, ESPIO_COMPONENT_DISPLAY, native, msg)

/**
 * @brief Set display error context without returning.
 */
#define ESPIO_DISPLAY_SET_ERROR(code, native, msg) \
    ESPIO_ERR_SET(code, ESPIO_COMPONENT_DISPLAY, native, msg)

#ifdef __cplusplus
}
#endif

#endif // ESPIO_DISPLAY_ERR_H
