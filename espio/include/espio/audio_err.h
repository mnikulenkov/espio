/**
 * @file err_audio.h
 * @brief Audio component specific error codes.
 * 
 * This header defines error codes specific to the audio component.
 * Error codes use a two-level identification system:
 * - The component field (ERR_COMPONENT_AUDIO) identifies the source
 * - The code field identifies the specific error
 * 
 * Error code ranges:
 * - 0: Success
 * - 1-99: Reserved for common errors (use ERR_* from err.h)
 * - 100-999: Audio component errors
 * - 1000+: Driver-specific errors
 */

#ifndef ESPIO_AUDIO_ERR_H
#define ESPIO_AUDIO_ERR_H

#include "espio/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Audio Error Codes
 *============================================================================*/

/**
 * @brief Audio-specific error codes.
 * 
 * Error codes start from 100 to leave room for common errors (0-99).
 * The component field in err_context_t identifies this as an audio error.
 */
typedef enum {
    // Success
    ESPIO_AUDIO_OK = 0,
    
    // Audio-specific errors (start from 100)
    ESPIO_AUDIO_ERR_DRIVER_FAILED = 100,    ///< Driver initialization failed
    ESPIO_AUDIO_ERR_I2S_CONFIG = 101,       ///< I2S configuration error
    ESPIO_AUDIO_ERR_I2S_INIT = 102,         ///< I2S initialization failed
    ESPIO_AUDIO_ERR_I2S_WRITE = 103,        ///< I2S write operation failed
    ESPIO_AUDIO_ERR_AMP_INIT = 104,         ///< Amplifier initialization failed
    ESPIO_AUDIO_ERR_DAC_INIT = 105,         ///< DAC initialization failed
    ESPIO_AUDIO_ERR_DAC_CONFIG = 106,       ///< DAC configuration error
    ESPIO_AUDIO_ERR_NOT_POWERED = 107,      ///< Amplifier not powered
    ESPIO_AUDIO_ERR_MUTED = 108,            ///< Amplifier is muted
    ESPIO_AUDIO_ERR_VOLUME = 109,           ///< Volume operation failed
    ESPIO_AUDIO_ERR_INVALID_FORMAT = 110,   ///< Invalid audio format
    ESPIO_AUDIO_ERR_SAMPLE_RATE = 111,      ///< Unsupported sample rate
    ESPIO_AUDIO_ERR_CHANNEL_MODE = 112,     ///< Unsupported channel mode
    ESPIO_AUDIO_ERR_BIT_WIDTH = 113,        ///< Unsupported bit width
    ESPIO_AUDIO_ERR_BUFFER_UNDERRUN = 114,  ///< Audio buffer underrun
    ESPIO_AUDIO_ERR_NOT_STARTED = 115,      ///< Audio not started
    ESPIO_AUDIO_ERR_GPIO = 116,             ///< GPIO configuration/operation failed
    
    // Driver-specific errors (drivers can add codes starting from 1000)
    ESPIO_AUDIO_ERR_DRIVER_BASE = 1000,     ///< Base for driver-specific errors
} espio_audio_err_t;

/*============================================================================
 * Helper Functions
 *============================================================================*/

/**
 * @brief Check if audio error code indicates success.
 * 
 * @param code Error code to check
 * @return true if the code indicates success (AUDIO_OK)
 */
static inline bool espio_audio_is_ok(int32_t code) {
    return code == ESPIO_AUDIO_OK;
}

/**
 * @brief Get human-readable name for an audio error code.
 * 
 * @param code Error code
 * @return Static string name, or NULL if not an audio-specific error
 */
const char* espio_audio_err_name(int32_t code);

/**
 * @brief Get human-readable description for an audio error code.
 * 
 * @param code Error code
 * @return Static string description, or NULL if not an audio-specific error
 */
const char* espio_audio_err_desc(int32_t code);

/*============================================================================
 * Error Context Macros
 *============================================================================*/

/**
 * @brief Create audio error context.
 */
#define ESPIO_AUDIO_ERR_CONTEXT(code, native, msg) \
    ESPIO_ERR_CONTEXT(code, ESPIO_COMPONENT_AUDIO, native, msg)

#define ESPIO_AUDIO_ERR_CONTEXT_HERE(code, native, msg) \
    ESPIO_ERR_CONTEXT_HERE(code, ESPIO_COMPONENT_AUDIO, native, msg)

/*============================================================================
 * Return Macros
 *============================================================================*/

/**
 * @brief Return audio error with thread-local context.
 * 
 * Sets the thread-local error context and returns the error code.
 * 
 * @param code Error code (use `ESPIO_AUDIO_*` or `ESPIO_*` constants)
 * @param native Native error code (e.g., esp_err_t, or 0)
 * @param msg Human-readable error message (static string)
 */
#define ESPIO_AUDIO_RETURN(code, native, msg) \
    ESPIO_ERR_RETURN(code, ESPIO_COMPONENT_AUDIO, native, msg)

/**
 * @brief Return audio success.
 */
#define ESPIO_AUDIO_RETURN_OK() \
    ESPIO_ERR_RETURN_OK(ESPIO_COMPONENT_AUDIO)

/**
 * @brief Return audio error if condition is true.
 */
#define ESPIO_AUDIO_RETURN_IF(cond, code, native, msg) \
    ESPIO_ERR_RETURN_IF(cond, code, ESPIO_COMPONENT_AUDIO, native, msg)

/**
 * @brief Set audio error context without returning.
 */
#define ESPIO_AUDIO_SET_ERROR(code, native, msg) \
    ESPIO_ERR_SET(code, ESPIO_COMPONENT_AUDIO, native, msg)

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AUDIO_ERR_H */
