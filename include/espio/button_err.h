/**
 * @file err_button.h
 * @brief Button component specific error codes.
 * 
 * This header defines error codes specific to the button component.
 * Error codes use a two-level identification system:
 * - The component field (ERR_COMPONENT_BUTTON) identifies the source
 * - The code field identifies the specific error
 * 
 * Error code ranges:
 * - 0: Success
 * - 1-99: Reserved for common errors (use ERR_* from err.h)
 * - 100-999: Button component errors
 * - 1000+: Provider/driver-specific errors
 */

#ifndef ESPIO_BUTTON_ERR_H
#define ESPIO_BUTTON_ERR_H

#include "espio/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Button Error Codes
 *============================================================================*/

/**
 * @brief Button-specific error codes.
 * 
 * Error codes start from 100 to leave room for common errors (0-99).
 * The component field in err_context_t identifies this as a button error.
 */
typedef enum {
    // Success
    ESPIO_BUTTON_OK = 0,
    
    // Button-specific errors (start from 100)
    ESPIO_BUTTON_ERR_QUEUE_FULL = 100,       ///< Event queue overflow
    ESPIO_BUTTON_ERR_DRIVER = 101,           ///< Provider driver failure
    ESPIO_BUTTON_ERR_GPIO_CONFIG = 102,      ///< GPIO configuration failed
    ESPIO_BUTTON_ERR_ISR_INSTALL = 103,      ///< ISR service installation failed
    ESPIO_BUTTON_ERR_PROVIDER_LIMIT = 104,   ///< Maximum providers reached
    ESPIO_BUTTON_ERR_BINDING_FAILED = 105,   ///< Binding creation failed
    ESPIO_BUTTON_ERR_INVALID_PROVIDER = 106, ///< Invalid provider configuration
    ESPIO_BUTTON_ERR_INVALID_BINDING = 107,  ///< Invalid binding configuration
    ESPIO_BUTTON_ERR_DUPLICATE_ID = 108,     ///< Duplicate button ID
    ESPIO_BUTTON_ERR_NOT_RUNNING = 109,      ///< Manager not running
    ESPIO_BUTTON_ERR_ALREADY_RUNNING = 110,  ///< Manager already running
    ESPIO_BUTTON_ERR_STOP_FAILED = 111,      ///< Failed to stop manager
    
    // Provider/driver-specific errors (drivers can add codes starting from 1000)
    ESPIO_BUTTON_ERR_DRIVER_BASE = 1000,     ///< Base for driver-specific errors
} espio_button_err_t;

/*============================================================================
 * Helper Functions
 *============================================================================*/

/**
 * @brief Check if button error code indicates success.
 * 
 * @param code Error code to check
 * @return true if the code indicates success (BUTTON_OK)
 */
static inline bool espio_button_is_ok(int32_t code) {
    return code == ESPIO_BUTTON_OK;
}

/**
 * @brief Get human-readable name for a button error code.
 * 
 * @param code Error code
 * @return Static string name, or NULL if not a button-specific error
 */
const char* espio_button_err_name(int32_t code);

/**
 * @brief Get human-readable description for a button error code.
 * 
 * @param code Error code
 * @return Static string description, or NULL if not a button-specific error
 */
const char* espio_button_err_desc(int32_t code);

/*============================================================================
 * Error Context Macros
 *============================================================================*/

/**
 * @brief Create button error context.
 */
#define ESPIO_BUTTON_ERR_CONTEXT(code, native, msg) \
    ESPIO_ERR_CONTEXT(code, ESPIO_COMPONENT_BUTTON, native, msg)

#define ESPIO_BUTTON_ERR_CONTEXT_HERE(code, native, msg) \
    ESPIO_ERR_CONTEXT_HERE(code, ESPIO_COMPONENT_BUTTON, native, msg)

/*============================================================================
 * Return Macros
 *============================================================================*/

/**
 * @brief Return button error with thread-local context.
 * 
 * Sets the thread-local error context and returns the error code.
 * 
 * @param code Error code (use `ESPIO_BUTTON_*` or `ESPIO_*` constants)
 * @param native Native error code (e.g., esp_err_t, or 0)
 * @param msg Human-readable error message (static string)
 */
#define ESPIO_BUTTON_RETURN(code, native, msg) \
    ESPIO_ERR_RETURN(code, ESPIO_COMPONENT_BUTTON, native, msg)

/**
 * @brief Return button success.
 */
#define ESPIO_BUTTON_RETURN_OK() \
    ESPIO_ERR_RETURN_OK(ESPIO_COMPONENT_BUTTON)

/**
 * @brief Return button error if condition is true.
 */
#define ESPIO_BUTTON_RETURN_IF(cond, code, native, msg) \
    ESPIO_ERR_RETURN_IF(cond, code, ESPIO_COMPONENT_BUTTON, native, msg)

/**
 * @brief Set button error context without returning.
 */
#define ESPIO_BUTTON_SET_ERROR(code, native, msg) \
    ESPIO_ERR_SET(code, ESPIO_COMPONENT_BUTTON, native, msg)

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_BUTTON_ERR_H */
