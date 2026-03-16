/**
 * @file audio_dac.h
 * @brief DAC dependency contract for analog-input amplifiers.
 */

#ifndef ESPIO_AUDIO_DAC_H
#define ESPIO_AUDIO_DAC_H

#include "espio/audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque DAC handle.
 */
typedef struct espio_audio_dac espio_audio_dac_t;

/**
 * @brief DAC write contract used by future analog-input amplifier drivers.
 *
 * The amplifier layer does not own PCM conversion. It consumes a DAC object that
 * already knows how to accept configured PCM samples and push them to analog output.
 */
typedef struct {
    /**
     * @brief Configure the DAC for a PCM format.
     * @param dac DAC instance.
     * @param format Requested PCM format.
     * @return 0 on success, negative on failure.
     */
    int (*configure)(espio_audio_dac_t* dac, const espio_audio_format_t* format);

    /**
     * @brief Write PCM data through the DAC.
     * @param dac DAC instance.
     * @param data PCM payload.
     * @param size PCM payload size in bytes.
     * @param bytes_written Output number of written bytes, when non-NULL.
     * @param timeout_ms Write timeout in milliseconds.
     * @return 0 on success, negative on failure.
     */
    int (*write)(espio_audio_dac_t* dac, const void* data, size_t size, size_t* bytes_written, uint32_t timeout_ms);
} espio_audio_dac_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AUDIO_DAC_H */
