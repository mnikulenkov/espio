/**
 * @file audio_mic.h
 * @brief Generic microphone lifecycle and RX API.
 */

#ifndef ESPIO_AUDIO_MIC_H
#define ESPIO_AUDIO_MIC_H

#include "espio/audio_err.h"
#include "espio/mic_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque microphone handle.
 */
typedef struct espio_audio_mic espio_audio_mic_t;

/**
 * @brief Opaque microphone RX transport handle.
 */
typedef struct espio_audio_mic_rx espio_audio_mic_rx_t;

/**
 * @brief Generic microphone creation parameters.
 */
typedef struct {
    const espio_audio_mic_driver_t* driver; ///< Concrete microphone driver.
    const void* driver_config;      ///< Driver-specific configuration.
    bool enable_software_gain;      ///< Allow software gain when the microphone lacks native gain control.
} espio_audio_mic_config_t;

/**
 * @brief Microphone RX transport configuration.
 */
typedef struct {
    uint32_t read_timeout_ms;       ///< Default blocking timeout for reads.
    size_t buffer_size;             ///< Reserved internal buffer size for future transports.
} espio_audio_mic_rx_config_t;

/**
 * @brief Create and initialize a microphone handle.
 * @param config Generic microphone configuration.
 * @return Microphone handle on success, NULL on failure.
 */
espio_audio_mic_t* espio_audio_mic_create(const espio_audio_mic_config_t* config);

/**
 * @brief Destroy a microphone handle.
 * @param mic Microphone handle.
 */
void espio_audio_mic_destroy(espio_audio_mic_t* mic);

/**
 * @brief Get microphone capabilities.
 * @param mic Microphone handle.
 * @return Pointer to immutable capabilities, or NULL on failure.
 */
const espio_audio_mic_caps_t* espio_audio_mic_get_caps(const espio_audio_mic_t* mic);

/**
 * @brief Get the microphone interface family.
 * @param mic Microphone handle.
 * @return Interface family, or `ESPIO_AUDIO_MIC_INTERFACE_I2S` when unavailable.
 */
espio_audio_mic_interface_type_t espio_audio_mic_get_interface(const espio_audio_mic_t* mic);

/**
 * @brief Set microphone power state.
 * @param mic Microphone handle.
 * @param on true to enable capture, false to disable it.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_set_power(espio_audio_mic_t* mic, bool on);

/**
 * @brief Set microphone gain.
 * @param mic Microphone handle.
 * @param gain_db Requested gain in dB.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_set_gain(espio_audio_mic_t* mic, int8_t gain_db);

/**
 * @brief Get the current microphone gain.
 * @param mic Microphone handle.
 * @param gain_db Output gain in dB.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_get_gain(const espio_audio_mic_t* mic, int8_t* gain_db);

/**
 * @brief Create and configure a microphone RX helper.
 * @param mic Microphone handle.
 * @param config RX configuration.
 * @return RX handle on success, NULL on failure.
 */
espio_audio_mic_rx_t* espio_audio_mic_rx_create(espio_audio_mic_t* mic, const espio_audio_mic_rx_config_t* config);

/**
 * @brief Destroy a microphone RX helper.
 * @param rx RX handle.
 */
void espio_audio_mic_rx_destroy(espio_audio_mic_rx_t* rx);

/**
 * @brief Start the RX transport path.
 * @param rx RX handle.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_start(espio_audio_mic_rx_t* rx);

/**
 * @brief Stop the RX transport path.
 * @param rx RX handle.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_stop(espio_audio_mic_rx_t* rx);

/**
 * @brief Read PCM data from the RX transport.
 * @param rx RX handle.
 * @param samples PCM destination buffer.
 * @param size Destination size in bytes.
 * @param bytes_read Output number of bytes read, when non-NULL.
 * @param timeout_ms Read timeout in milliseconds. Zero uses the configured default.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_read(espio_audio_mic_rx_t* rx, void* samples, size_t size, size_t* bytes_read, uint32_t timeout_ms);

/**
 * @brief Enable or disable the software high-pass filter.
 * @param rx RX handle.
 * @param enable true to enable the software high-pass filter.
 * @param cutoff_hz Requested cutoff in Hertz.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_enable_high_pass(espio_audio_mic_rx_t* rx, bool enable, float cutoff_hz);

/**
 * @brief Set an additional software gain multiplier for captured samples.
 * @param rx RX handle.
 * @param gain_linear Linear gain multiplier.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_set_software_gain(espio_audio_mic_rx_t* rx, float gain_linear);

/**
 * @brief Enable or disable software AGC.
 * @param rx RX handle.
 * @param enable true to enable the software AGC stage.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_enable_agc(espio_audio_mic_rx_t* rx, bool enable);

/**
 * @brief Set the AGC target level.
 * @param rx RX handle.
 * @param target_level_db Target level in dBFS.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_mic_rx_set_agc_target(espio_audio_mic_rx_t* rx, int8_t target_level_db);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AUDIO_MIC_H */
