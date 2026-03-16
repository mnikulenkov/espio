/**
 * @file audio_amp.h
 * @brief Generic audio amplifier and transport API.
 */

#ifndef ESPIO_AUDIO_AMP_H
#define ESPIO_AUDIO_AMP_H

#include "espio/audio_dac.h"
#include "espio/audio_types.h"
#include "espio/audio_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque amplifier handle.
 */
typedef struct espio_audio espio_audio_t;

/**
 * @brief Opaque PCM transport handle.
 */
typedef struct espio_audio_tx espio_audio_tx_t;

/**
 * @brief Driver contract implemented by concrete amplifier drivers.
 */
typedef struct espio_audio_driver {
    const char* name;   ///< Driver name for logs and diagnostics.

    /**
     * @brief Query amplifier capabilities for a specific driver configuration.
     * @param config Driver-specific configuration.
     * @param caps Output capability structure.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*query_caps)(const void* config, espio_audio_caps_t* caps);

    /**
     * @brief Initialize driver-specific state.
     * @param config Driver-specific configuration.
     * @return Driver context on success, NULL on failure.
     */
    void* (*init)(const void* config);

    /**
     * @brief Deinitialize driver-specific state.
     * @param ctx Driver context.
     */
    void (*deinit)(void* ctx);

    /**
     * @brief Set amplifier power state.
     * @param ctx Driver context.
     * @param on true to enable output, false to disable it.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*set_power)(void* ctx, bool on);

    /**
     * @brief Set amplifier mute state.
     * @param ctx Driver context.
     * @param mute true to mute output, false to unmute it.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*set_mute)(void* ctx, bool mute);

    /**
     * @brief Set hardware amplifier volume.
     * @param ctx Driver context.
     * @param percent Volume in the 0-100 range.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*set_volume)(void* ctx, uint8_t percent);
} espio_audio_driver_t;

/**
 * @brief Generic amplifier creation parameters.
 */
typedef struct {
    const espio_audio_driver_t* driver; ///< Concrete amplifier driver.
    const void* driver_config;        ///< Driver-specific configuration.
    espio_audio_dac_t* dac;           ///< Optional DAC dependency for analog-input amplifiers.
    bool enable_software_volume;      ///< Allow software gain when the amplifier lacks native volume control.
} espio_audio_config_t;

/**
 * @brief Create and initialize an audio amplifier handle.
 * @param config Generic amplifier configuration.
 * @return Amplifier handle on success, NULL on failure.
 */
espio_audio_t* espio_audio_create(const espio_audio_config_t* config);

/**
 * @brief Destroy an amplifier handle.
 * @param audio Amplifier handle.
 */
void espio_audio_destroy(espio_audio_t* audio);

/**
 * @brief Get amplifier capabilities.
 * @param audio Amplifier handle.
 * @return Pointer to immutable capabilities, or NULL on failure.
 */
const espio_audio_caps_t* espio_audio_get_caps(const espio_audio_t* audio);

/**
 * @brief Set amplifier power state.
 * @param audio Amplifier handle.
 * @param on true to enable output, false to disable it.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_set_power(espio_audio_t* audio, bool on);

/**
 * @brief Set amplifier mute state.
 * @param audio Amplifier handle.
 * @param mute true to mute output, false to unmute it.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_set_mute(espio_audio_t* audio, bool mute);

/**
 * @brief Set amplifier volume.
 *
 * When the amplifier lacks native volume control and software volume is enabled,
 * the requested value is applied during `espio_audio_tx_write()`.
 *
 * @param audio Amplifier handle.
 * @param percent Requested volume in the 0-100 range.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_set_volume(espio_audio_t* audio, uint8_t percent);

/**
 * @brief Get the current logical volume.
 * @param audio Amplifier handle.
 * @return Current logical volume in the 0-100 range, or 0 on failure.
 */
uint8_t espio_audio_get_volume(const espio_audio_t* audio);

/**
 * @brief Create and configure a PCM transmit helper.
 * @param audio Amplifier handle.
 * @param config Transport configuration.
 * @return Transport handle on success, NULL on failure.
 */
espio_audio_tx_t* espio_audio_tx_create(espio_audio_t* audio, const espio_audio_tx_config_t* config);

/**
 * @brief Destroy a PCM transmit helper.
 * @param tx Transport handle.
 */
void espio_audio_tx_destroy(espio_audio_tx_t* tx);

/**
 * @brief Start the transport path.
 * @param tx Transport handle.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_tx_start(espio_audio_tx_t* tx);

/**
 * @brief Stop the transport path.
 * @param tx Transport handle.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_tx_stop(espio_audio_tx_t* tx);

/**
 * @brief Write PCM data to the transport path.
 * @param tx Transport handle.
 * @param samples PCM payload.
 * @param size PCM payload size in bytes.
 * @param bytes_written Output number of bytes written, when non-NULL.
 * @param timeout_ms Write timeout in milliseconds. Zero uses the configured default.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_tx_write(espio_audio_tx_t* tx, const void* samples, size_t size, size_t* bytes_written, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AUDIO_AMP_H */
