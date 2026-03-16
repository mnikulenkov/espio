/**
 * @file amp_tx.c
 * @brief PCM transmit helper with optional software gain.
 */

#include "amp_internal.h"
#include "espio/audio_err.h"
#include "esp_heap_caps.h"
#include "espio/log.h"
#include <limits.h>
#include <string.h>

static const char* TAG = "audio_tx";

/**
 * @brief Validate a requested PCM format against amplifier capabilities.
 */
static int32_t audio_tx_validate_format(const espio_audio_caps_t* caps, const espio_audio_format_t* format) {
    if (!caps || !format) {
        return ESPIO_INVALID_ARG;
    }

    if (caps->input_class != ESPIO_AUDIO_INPUT_CLASS_DIGITAL) {
        return ESPIO_AUDIO_ERR_INVALID_FORMAT;
    }

    if (format->sample_rate_hz < caps->min_sample_rate_hz || format->sample_rate_hz > caps->max_sample_rate_hz) {
        return ESPIO_AUDIO_ERR_SAMPLE_RATE;
    }

    bool bit_width_supported = false;
    for (size_t i = 0; i < caps->supported_bit_width_count; i++) {
        if (caps->supported_bit_widths[i] == format->bit_width) {
            bit_width_supported = true;
            break;
        }
    }

    if (!bit_width_supported) {
        return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }

    bool serial_mode_supported = false;
    for (size_t i = 0; i < caps->supported_serial_mode_count; i++) {
        if (caps->supported_serial_modes[i] == format->serial_mode) {
            serial_mode_supported = true;
            break;
        }
    }

    if (!serial_mode_supported) {
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Return the packed sample size for the selected format.
 */
static size_t audio_tx_sample_size_bytes(espio_audio_bit_width_t bit_width) {
    switch (bit_width) {
        case ESPIO_AUDIO_BIT_WIDTH_16:
            return sizeof(int16_t);
        case ESPIO_AUDIO_BIT_WIDTH_24:
            return 3U;
        case ESPIO_AUDIO_BIT_WIDTH_32:
            return sizeof(int32_t);
        default:
            return 0U;
    }
}

/**
 * @brief Clamp an intermediate value into the signed 16-bit range.
 */
static int16_t audio_tx_clamp_i16(int32_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

/**
 * @brief Clamp an intermediate value into the signed 24-bit range.
 */
static int32_t audio_tx_clamp_i24(int64_t sample) {
    static const int32_t max_i24 = 0x7FFFFF;
    static const int32_t min_i24 = -0x800000;

    if (sample > max_i24) {
        return max_i24;
    }
    if (sample < min_i24) {
        return min_i24;
    }
    return (int32_t)sample;
}

/**
 * @brief Apply software gain to packed 16-bit PCM samples.
 */
static void audio_tx_apply_gain_16(uint8_t* data, size_t sample_count, uint8_t volume_percent) {
    int16_t* samples = (int16_t*)data;

    for (size_t i = 0; i < sample_count; i++) {
        int32_t scaled = ((int32_t)samples[i] * volume_percent) / 100;
        samples[i] = audio_tx_clamp_i16(scaled);
    }
}

/**
 * @brief Apply software gain to packed 24-bit PCM samples.
 */
static void audio_tx_apply_gain_24(uint8_t* data, size_t sample_count, uint8_t volume_percent) {
    for (size_t i = 0; i < sample_count; i++) {
        uint8_t* sample_bytes = &data[i * 3U];
        int32_t sample = (int32_t)sample_bytes[0] | ((int32_t)sample_bytes[1] << 8) | ((int32_t)sample_bytes[2] << 16);
        if ((sample & 0x00800000) != 0) {
            sample |= ~0x00FFFFFF;
        }

        int32_t scaled = audio_tx_clamp_i24(((int64_t)sample * volume_percent) / 100);
        sample_bytes[0] = (uint8_t)(scaled & 0xFF);
        sample_bytes[1] = (uint8_t)((scaled >> 8) & 0xFF);
        sample_bytes[2] = (uint8_t)((scaled >> 16) & 0xFF);
    }
}

/**
 * @brief Apply software gain to packed 32-bit PCM samples.
 */
static void audio_tx_apply_gain_32(uint8_t* data, size_t sample_count, uint8_t volume_percent) {
    int32_t* samples = (int32_t*)data;

    for (size_t i = 0; i < sample_count; i++) {
        int64_t scaled = ((int64_t)samples[i] * volume_percent) / 100;
        if (scaled > INT32_MAX) {
            scaled = INT32_MAX;
        } else if (scaled < INT32_MIN) {
            scaled = INT32_MIN;
        }
        samples[i] = (int32_t)scaled;
    }
}

/**
 * @brief Ensure the scratch buffer is large enough for software gain.
 */
static int32_t audio_tx_ensure_scratch_buffer(espio_audio_tx_t* tx, size_t size) {
    if (!tx) {
        return ESPIO_INVALID_ARG;
    }

    if (tx->scratch_buffer_size >= size) {
        return ESPIO_AUDIO_OK;
    }

    uint8_t* new_buffer = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!new_buffer) {
        ESPIO_LOGE(TAG, "failed to allocate scratch buffer");
        return ESPIO_NO_MEM;
    }

    if (tx->scratch_buffer) {
        heap_caps_free(tx->scratch_buffer);
    }

    tx->scratch_buffer = new_buffer;
    tx->scratch_buffer_size = size;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Copy PCM data and apply software gain for the configured format.
 */
static int32_t audio_tx_prepare_software_gain(espio_audio_tx_t* tx, const void* samples, size_t size, uint8_t volume_percent, const void** prepared_samples) {
    if (!tx || !samples || !prepared_samples) {
        return ESPIO_INVALID_ARG;
    }

    size_t sample_size = audio_tx_sample_size_bytes(tx->format.bit_width);
    if (sample_size == 0U || (size % sample_size) != 0U) {
        ESPIO_LOGE(TAG, "PCM payload size does not match the configured format");
        return ESPIO_AUDIO_ERR_INVALID_FORMAT;
    }

    if (audio_tx_ensure_scratch_buffer(tx, size) != ESPIO_AUDIO_OK) {
        return ESPIO_NO_MEM;
    }

    memcpy(tx->scratch_buffer, samples, size);

    size_t sample_count = size / sample_size;
    switch (tx->format.bit_width) {
        case ESPIO_AUDIO_BIT_WIDTH_16:
            audio_tx_apply_gain_16(tx->scratch_buffer, sample_count, volume_percent);
            break;
        case ESPIO_AUDIO_BIT_WIDTH_24:
            audio_tx_apply_gain_24(tx->scratch_buffer, sample_count, volume_percent);
            break;
        case ESPIO_AUDIO_BIT_WIDTH_32:
            audio_tx_apply_gain_32(tx->scratch_buffer, sample_count, volume_percent);
            break;
        default:
            return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }

    *prepared_samples = tx->scratch_buffer;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Create and configure a PCM transmit helper.
 */
espio_audio_tx_t* espio_audio_tx_create(espio_audio_t* audio, const espio_audio_tx_config_t* config) {
    if (!audio || !config) {
        return NULL;
    }

    if (audio_tx_validate_format(&audio->caps, &config->format) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "requested format is unsupported by amplifier capabilities");
        return NULL;
    }

    espio_audio_tx_config_t effective_config = *config;
    if (effective_config.mclk_policy == ESPIO_AUDIO_MCLK_POLICY_AUTO) {
        effective_config.mclk_policy = audio->caps.mclk_policy;
    }

    espio_audio_tx_t* tx = heap_caps_calloc(1, sizeof(espio_audio_tx_t), MALLOC_CAP_DEFAULT);
    if (!tx) {
        ESPIO_LOGE(TAG, "failed to allocate audio transport");
        return NULL;
    }

    tx->audio = audio;
    tx->format = config->format;
    tx->default_timeout_ms = config->write_timeout_ms;

    if (audio_i2s_hal_init_tx(&tx->hal,
                              &effective_config.format,
                              &effective_config.pins,
                              effective_config.controller_id,
                              effective_config.mclk_policy) != ESPIO_AUDIO_OK) {
        heap_caps_free(tx);
        return NULL;
    }

    ESPIO_LOGI(TAG, "audio transport created");
    return tx;
}

/**
 * @brief Destroy a PCM transmit helper.
 */
void espio_audio_tx_destroy(espio_audio_tx_t* tx) {
    if (!tx) {
        return;
    }

    audio_i2s_hal_deinit(&tx->hal);

    if (tx->scratch_buffer) {
        heap_caps_free(tx->scratch_buffer);
    }

    heap_caps_free(tx);
}

/**
 * @brief Start the transport path.
 */
int32_t espio_audio_tx_start(espio_audio_tx_t* tx) {
    if (!tx) {
        return ESPIO_INVALID_ARG;
    }

    if (audio_i2s_hal_start_tx(&tx->hal) != ESPIO_AUDIO_OK) {
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    tx->started = true;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Stop the transport path.
 */
int32_t espio_audio_tx_stop(espio_audio_tx_t* tx) {
    if (!tx) {
        return ESPIO_INVALID_ARG;
    }

    if (audio_i2s_hal_stop_tx(&tx->hal) != ESPIO_AUDIO_OK) {
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    tx->started = false;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Write PCM data to the transport path.
 */
int32_t espio_audio_tx_write(espio_audio_tx_t* tx, const void* samples, size_t size, size_t* bytes_written, uint32_t timeout_ms) {
    if (!tx || !samples || size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    if (!tx->started) {
        ESPIO_LOGE(TAG, "audio transport is not started");
        return ESPIO_AUDIO_ERR_NOT_STARTED;
    }

    uint32_t effective_timeout_ms = timeout_ms == 0U ? tx->default_timeout_ms : timeout_ms;
    uint8_t effective_volume = espio_audio_internal_effective_volume(tx->audio);

    const void* payload = samples;
    if (tx->audio->software_volume_enabled && effective_volume < 100U) {
        if (audio_tx_prepare_software_gain(tx, samples, size, effective_volume, &payload) != ESPIO_AUDIO_OK) {
            return ESPIO_AUDIO_ERR_VOLUME;
        }
    }

    return audio_i2s_hal_write(&tx->hal, payload, size, bytes_written, effective_timeout_ms);
}
