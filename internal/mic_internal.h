/**
 * @file mic_internal.h
 * @brief Private shared declarations for the audio mic component.
 */

#ifndef ESPIO_AUDIO_MIC_INTERNAL_H
#define ESPIO_AUDIO_MIC_INTERNAL_H

#include "audio_i2s_hal.h"
#include "espio/audio_mic.h"
#include "esp_adc/adc_continuous.h"
#include <stdbool.h>

#define ESPIO_AUDIO_MIC_INTERNAL_MAX_CHANNELS 8U

/**
 * @brief Internal RX backend families.
 */
typedef enum {
    MIC_RX_BACKEND_NONE = 0,
    MIC_RX_BACKEND_I2S = 1,
    MIC_RX_BACKEND_ADC = 2,
} mic_rx_backend_t;

/**
 * @brief ADC transport state kept by the generic RX layer.
 */
typedef struct {
    adc_continuous_handle_t handle;    ///< ESP-IDF ADC continuous handle.
    uint8_t* raw_buffer;               ///< Raw ADC DMA buffer.
    size_t raw_buffer_size;            ///< Raw ADC DMA buffer size in bytes.
    adc_continuous_data_t* parsed_buffer; ///< Parsed ADC samples.
    uint32_t parsed_capacity;          ///< Parsed ADC sample capacity.
    int adc_unit;                      ///< Selected ADC unit.
    int adc_channel;                   ///< Selected ADC channel.
} mic_adc_transport_t;

/**
 * @brief Internal microphone state.
 */
struct espio_audio_mic {
    const espio_audio_mic_driver_t* driver; ///< Concrete driver implementation.
    const void* driver_config;           ///< Immutable driver configuration.
    void* driver_ctx;                    ///< Driver-specific context.
    espio_audio_mic_caps_t caps;         ///< Effective capabilities for this instance.
    espio_audio_mic_transport_config_t transport_config; ///< Normalized transport configuration.
    bool software_gain_enabled;          ///< True when software gain may be applied.
    bool powered;                        ///< Last requested power state.
    int8_t gain_db;                      ///< Last requested logical gain in dB.
    float software_gain_linear;          ///< Base gain multiplier derived from `gain_db`.
};

/**
 * @brief Internal RX transport state.
 */
struct espio_audio_mic_rx {
    espio_audio_mic_t* mic;             ///< Bound microphone handle.
    mic_rx_backend_t backend;            ///< Active transport backend family.
    union {
        audio_i2s_hal_t i2s;             ///< Shared I2S transport wrapper.
        mic_adc_transport_t adc;         ///< ADC continuous transport wrapper.
    } transport;
    espio_audio_format_t format;         ///< Configured PCM format.
    uint8_t active_channel_count;        ///< Number of interleaved channels in one frame.
    uint32_t default_timeout_ms;         ///< Default read timeout.
    bool started;                        ///< True when the RX path is enabled.
    bool high_pass_enabled;              ///< True when software HPF is enabled.
    float high_pass_cutoff_hz;           ///< HPF cutoff in Hertz.
    bool agc_enabled;                    ///< True when software AGC is enabled.
    int8_t agc_target_level_db;          ///< Target AGC level in dBFS.
    float agc_gain_linear;               ///< Smoothed AGC multiplier.
    float extra_gain_linear;             ///< Additional user-selected software gain multiplier.
    float high_pass_prev_input[ESPIO_AUDIO_MIC_INTERNAL_MAX_CHANNELS];  ///< Previous HPF input sample per channel.
    float high_pass_prev_output[ESPIO_AUDIO_MIC_INTERNAL_MAX_CHANNELS]; ///< Previous HPF output sample per channel.
};

/**
 * @brief Convert a dB gain request into a linear multiplier.
 * @param gain_db Gain in dB.
 * @return Linear multiplier.
 */
float espio_audio_mic_internal_db_to_linear(int8_t gain_db);

/**
 * @brief Resolve the base software gain configured on the microphone.
 * @param mic Microphone handle.
 * @return Linear gain multiplier.
 */
float espio_audio_mic_internal_effective_gain_linear(const espio_audio_mic_t* mic);

#endif /* ESPIO_AUDIO_MIC_INTERNAL_H */
