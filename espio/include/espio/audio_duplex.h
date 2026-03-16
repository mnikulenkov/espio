/**
 * @file audio_duplex.h
 * @brief Full-duplex PCM transport API.
 */

#ifndef ESPIO_AUDIO_DUPLEX_H
#define ESPIO_AUDIO_DUPLEX_H

#include "espio/audio_err.h"
#include "espio/audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque full-duplex transport handle.
 */
typedef struct espio_audio_duplex espio_audio_duplex_t;

/**
 * @brief Full-duplex I2S transport configuration.
 */
typedef struct {
    espio_audio_format_t format;       ///< Shared PCM format used by TX and RX.
    espio_audio_i2s_pins_t pins;       ///< Shared I2S pin assignment.
    int controller_id;                 ///< I2S controller index, or a negative value for auto-selection.
    uint32_t read_timeout_ms;          ///< Default RX timeout in milliseconds.
    uint32_t write_timeout_ms;         ///< Default TX timeout in milliseconds.
    espio_audio_mclk_policy_t mclk_policy; ///< Requested MCLK policy.
} espio_audio_duplex_config_t;

/**
 * @brief Create a full-duplex I2S transport.
 * @param config Duplex configuration.
 * @return Duplex handle on success, NULL on failure.
 */
espio_audio_duplex_t* espio_audio_duplex_create(const espio_audio_duplex_config_t* config);

/**
 * @brief Destroy a full-duplex transport.
 * @param duplex Duplex handle.
 */
void espio_audio_duplex_destroy(espio_audio_duplex_t* duplex);

/**
 * @brief Start both TX and RX sides of the duplex transport.
 * @param duplex Duplex handle.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_duplex_start(espio_audio_duplex_t* duplex);

/**
 * @brief Stop both TX and RX sides of the duplex transport.
 * @param duplex Duplex handle.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_duplex_stop(espio_audio_duplex_t* duplex);

/**
 * @brief Read PCM data from the duplex RX side.
 * @param duplex Duplex handle.
 * @param samples Destination buffer.
 * @param size Destination size in bytes.
 * @param bytes_read Output number of bytes read, when non-NULL.
 * @param timeout_ms Read timeout in milliseconds. Zero uses the configured default.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_duplex_read(espio_audio_duplex_t* duplex, void* samples, size_t size, size_t* bytes_read, uint32_t timeout_ms);

/**
 * @brief Write PCM data to the duplex TX side.
 * @param duplex Duplex handle.
 * @param samples Source buffer.
 * @param size Source size in bytes.
 * @param bytes_written Output number of bytes written, when non-NULL.
 * @param timeout_ms Write timeout in milliseconds. Zero uses the configured default.
 * @return AUDIO_OK on success, or error code on failure.
 */
int32_t espio_audio_duplex_write(espio_audio_duplex_t* duplex, const void* samples, size_t size, size_t* bytes_written, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AUDIO_DUPLEX_H */
