/**
 * @file amp_internal.h
 * @brief Private shared declarations for the audio amp component.
 */

#ifndef ESPIO_AUDIO_INTERNAL_H
#define ESPIO_AUDIO_INTERNAL_H

#include "audio_i2s_hal.h"
#include "espio/audio_amp.h"

/**
 * @brief Internal amplifier state.
 */
struct espio_audio {
    const espio_audio_driver_t* driver; ///< Concrete driver implementation.
    void* driver_ctx;                 ///< Driver-specific context.
    espio_audio_caps_t caps;          ///< Effective capabilities for this instance.
    espio_audio_dac_t* dac;           ///< Optional DAC dependency for future analog-input drivers.
    bool software_volume_enabled;     ///< True when transport gain should be applied.
    bool powered;                     ///< Last requested power state.
    bool muted;                       ///< Last requested mute state.
    uint8_t volume_percent;           ///< Last requested logical volume.
};

/**
 * @brief Internal transport state.
 */
struct espio_audio_tx {
    espio_audio_t* audio;            ///< Bound amplifier handle.
    audio_i2s_hal_t hal;             ///< I2S transport wrapper.
    espio_audio_format_t format;     ///< Configured PCM format.
    uint32_t default_timeout_ms;     ///< Default write timeout.
    uint8_t* scratch_buffer;         ///< Scratch buffer for software gain.
    size_t scratch_buffer_size;      ///< Allocated scratch-buffer size.
    bool started;                    ///< True when the transport is enabled.
};

/**
 * @brief Resolve the effective runtime volume.
 *
 * The transmit helper needs one place to interpret mute and logical volume so
 * it can keep software gain behavior consistent across drivers.
 *
 * @param audio Amplifier handle.
 * @return Effective volume in the 0-100 range.
 */
uint8_t espio_audio_internal_effective_volume(const espio_audio_t* audio);

#endif /* ESPIO_AUDIO_INTERNAL_H */
