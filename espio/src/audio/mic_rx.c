/**
 * @file mic_rx.c
 * @brief PCM receive helper with optional software processing.
 */

#include "mic_internal.h"
#include "esp_heap_caps.h"
#include "espio/log.h"
#include "freertos/FreeRTOS.h"
#include <limits.h>
#include <math.h>

static const char* TAG = "audio_mic_rx";
static const float MIC_PI = 3.14159265358979323846f;
static const size_t MIC_ADC_DEFAULT_FRAME_SIZE = 256U;
static const size_t MIC_ADC_DEFAULT_STORE_SIZE = 1024U;

/**
 * @brief Destroy a microphone RX helper.
 */
void espio_audio_mic_rx_destroy(espio_audio_mic_rx_t* rx);

/**
 * @brief Validate a requested PCM format against microphone capabilities.
 */
static int32_t mic_rx_validate_format(const espio_audio_mic_caps_t* caps,
                                      const espio_audio_format_t* format,
                                      espio_audio_mic_interface_type_t interface_type) {
    if (!caps || !format) {
        return ESPIO_INVALID_ARG;
    }

    if ((interface_type == ESPIO_AUDIO_MIC_INTERFACE_I2S || interface_type == ESPIO_AUDIO_MIC_INTERFACE_TDM) &&
        format->serial_mode != ESPIO_AUDIO_SERIAL_MODE_I2S_PHILIPS) {
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

    if (format->channel_mode == ESPIO_AUDIO_CHANNEL_MODE_STEREO && !caps->supports_stereo) {
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Return the packed sample size for the selected format.
 */
static size_t mic_rx_sample_size_bytes(espio_audio_bit_width_t bit_width) {
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
static int16_t mic_rx_clamp_i16(float sample) {
    if (sample > (float)INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < (float)INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

/**
 * @brief Clamp an intermediate value into the signed 24-bit range.
 */
static int32_t mic_rx_clamp_i24(float sample) {
    static const float max_i24 = 8388607.0f;
    static const float min_i24 = -8388608.0f;

    if (sample > max_i24) {
        return 0x7FFFFF;
    }
    if (sample < min_i24) {
        return -0x800000;
    }
    return (int32_t)sample;
}

/**
 * @brief Decode one packed sample into a float accumulator.
 */
static float mic_rx_load_sample(const uint8_t* data, espio_audio_bit_width_t bit_width) {
    switch (bit_width) {
        case ESPIO_AUDIO_BIT_WIDTH_16:
            return (float)(*(const int16_t*)data);
        case ESPIO_AUDIO_BIT_WIDTH_24: {
            int32_t sample = (int32_t)data[0] | ((int32_t)data[1] << 8) | ((int32_t)data[2] << 16);
            if ((sample & 0x00800000) != 0) {
                sample |= ~0x00FFFFFF;
            }
            return (float)sample;
        }
        case ESPIO_AUDIO_BIT_WIDTH_32:
            return (float)(*(const int32_t*)data);
        default:
            return 0.0f;
    }
}

/**
 * @brief Encode one float accumulator into the packed target format.
 */
static void mic_rx_store_sample(uint8_t* data, espio_audio_bit_width_t bit_width, float sample) {
    switch (bit_width) {
        case ESPIO_AUDIO_BIT_WIDTH_16:
            *(int16_t*)data = mic_rx_clamp_i16(sample);
            return;
        case ESPIO_AUDIO_BIT_WIDTH_24: {
            int32_t packed = mic_rx_clamp_i24(sample);
            data[0] = (uint8_t)(packed & 0xFF);
            data[1] = (uint8_t)((packed >> 8) & 0xFF);
            data[2] = (uint8_t)((packed >> 16) & 0xFF);
            return;
        }
        case ESPIO_AUDIO_BIT_WIDTH_32:
            if (sample > (float)INT32_MAX) {
                sample = (float)INT32_MAX;
            } else if (sample < (float)INT32_MIN) {
                sample = (float)INT32_MIN;
            }
            *(int32_t*)data = (int32_t)sample;
            return;
        default:
            return;
    }
}

/**
 * @brief Choose the channel count used by the software processing path.
 */
static uint8_t mic_rx_resolve_channel_count(const espio_audio_mic_t* mic) {
    if (!mic) {
        return 0U;
    }

    switch (mic->transport_config.interface_type) {
        case ESPIO_AUDIO_MIC_INTERFACE_TDM:
            return mic->transport_config.config.tdm.channel_count;
        case ESPIO_AUDIO_MIC_INTERFACE_ADC:
            return 1U;
        case ESPIO_AUDIO_MIC_INTERFACE_I2S:
            return (uint8_t)mic->transport_config.config.i2s.format.channel_mode;
        case ESPIO_AUDIO_MIC_INTERFACE_PDM:
            return (uint8_t)mic->transport_config.config.pdm.format.channel_mode;
        default:
            return 0U;
    }
}

/**
 * @brief Apply software HPF, gain, and AGC in place on one PCM buffer.
 */
static int32_t mic_rx_process_samples(espio_audio_mic_rx_t* rx, void* samples, size_t size) {
    if (!rx || !samples || size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    size_t sample_size = mic_rx_sample_size_bytes(rx->format.bit_width);
    if (sample_size == 0U || (size % sample_size) != 0U) {
        return ESPIO_AUDIO_ERR_INVALID_FORMAT;
    }

    uint8_t channel_count = rx->active_channel_count;
    if (channel_count == 0U || channel_count > ESPIO_AUDIO_MIC_INTERNAL_MAX_CHANNELS) {
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    size_t sample_count = size / sample_size;
    const float dt = 1.0f / (float)rx->format.sample_rate_hz;
    float hpf_alpha = 0.0f;
    if (rx->high_pass_enabled && rx->high_pass_cutoff_hz > 0.0f) {
        const float rc = 1.0f / (2.0f * MIC_PI * rx->high_pass_cutoff_hz);
        hpf_alpha = rc / (rc + dt);
    }

    float peak = 0.0f;
    uint8_t* data = (uint8_t*)samples;
    for (size_t sample_index = 0; sample_index < sample_count; sample_index++) {
        uint8_t channel = (uint8_t)(sample_index % channel_count);
        float current = mic_rx_load_sample(&data[sample_index * sample_size], rx->format.bit_width);

        if (rx->high_pass_enabled && hpf_alpha > 0.0f) {
            float filtered = hpf_alpha * (rx->high_pass_prev_output[channel] + current - rx->high_pass_prev_input[channel]);
            rx->high_pass_prev_input[channel] = current;
            rx->high_pass_prev_output[channel] = filtered;
            current = filtered;
        }

        float abs_current = fabsf(current);
        if (abs_current > peak) {
            peak = abs_current;
        }

        mic_rx_store_sample(&data[sample_index * sample_size], rx->format.bit_width, current);
    }

    float total_gain = espio_audio_mic_internal_effective_gain_linear(rx->mic) * rx->extra_gain_linear;
    if (rx->agc_enabled && peak > 0.0f) {
        float max_level = 1.0f;
        switch (rx->format.bit_width) {
            case ESPIO_AUDIO_BIT_WIDTH_16:
                max_level = (float)INT16_MAX;
                break;
            case ESPIO_AUDIO_BIT_WIDTH_24:
                max_level = 8388607.0f;
                break;
            case ESPIO_AUDIO_BIT_WIDTH_32:
                max_level = (float)INT32_MAX;
                break;
            default:
                return ESPIO_AUDIO_ERR_BIT_WIDTH;
        }

        float target_peak = max_level * espio_audio_mic_internal_db_to_linear(rx->agc_target_level_db);
        float desired_gain = target_peak / peak;
        if (desired_gain < 0.125f) {
            desired_gain = 0.125f;
        } else if (desired_gain > 8.0f) {
            desired_gain = 8.0f;
        }

        rx->agc_gain_linear = (rx->agc_gain_linear * 0.8f) + (desired_gain * 0.2f);
        total_gain *= rx->agc_gain_linear;
    }

    if (fabsf(total_gain - 1.0f) < 0.0001f) {
        return ESPIO_AUDIO_OK;
    }

    for (size_t sample_index = 0; sample_index < sample_count; sample_index++) {
        float current = mic_rx_load_sample(&data[sample_index * sample_size], rx->format.bit_width);
        current *= total_gain;
        mic_rx_store_sample(&data[sample_index * sample_size], rx->format.bit_width, current);
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Convert one ADC attenuation selector into the ESP-IDF enum.
 */
static int32_t mic_rx_map_adc_atten(uint8_t attenuation_db, adc_atten_t* out_atten) {
    if (!out_atten) {
        return ESPIO_INVALID_ARG;
    }

    switch (attenuation_db) {
        case 0U:
            *out_atten = ADC_ATTEN_DB_0;
            return ESPIO_AUDIO_OK;
        case 2U:
            *out_atten = ADC_ATTEN_DB_2_5;
            return ESPIO_AUDIO_OK;
        case 6U:
            *out_atten = ADC_ATTEN_DB_6;
            return ESPIO_AUDIO_OK;
        case 12U:
            *out_atten = ADC_ATTEN_DB_12;
            return ESPIO_AUDIO_OK;
        default:
            return ESPIO_INVALID_ARG;
    }
}

/**
 * @brief Map the public ADC unit number into the ESP-IDF conversion mode.
 */
static int32_t mic_rx_map_adc_unit(int adc_unit, adc_digi_convert_mode_t* out_conv_mode) {
    if (!out_conv_mode) {
        return ESPIO_INVALID_ARG;
    }

    switch (adc_unit) {
        case 1:
            *out_conv_mode = ADC_CONV_SINGLE_UNIT_1;
            return ESPIO_AUDIO_OK;
        case 2:
            *out_conv_mode = ADC_CONV_SINGLE_UNIT_2;
            return ESPIO_AUDIO_OK;
        default:
            return ESPIO_INVALID_ARG;
    }
}

/**
 * @brief Allocate the internal ADC DMA and parsed buffers.
 */
static int32_t mic_rx_allocate_adc_buffers(espio_audio_mic_rx_t* rx, size_t raw_buffer_size) {
    if (!rx || raw_buffer_size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    rx->transport.adc.raw_buffer = heap_caps_malloc(raw_buffer_size, MALLOC_CAP_8BIT);
    if (!rx->transport.adc.raw_buffer) {
        ESPIO_LOGE(TAG, "failed to allocate ADC raw buffer");
        return ESPIO_NO_MEM;
    }

    rx->transport.adc.raw_buffer_size = raw_buffer_size;
    rx->transport.adc.parsed_capacity = (uint32_t)(raw_buffer_size / SOC_ADC_DIGI_RESULT_BYTES);
    rx->transport.adc.parsed_buffer = heap_caps_calloc(rx->transport.adc.parsed_capacity,
                                                       sizeof(adc_continuous_data_t),
                                                       MALLOC_CAP_DEFAULT);
    if (!rx->transport.adc.parsed_buffer) {
        ESPIO_LOGE(TAG, "failed to allocate ADC parsed buffer");
        heap_caps_free(rx->transport.adc.raw_buffer);
        rx->transport.adc.raw_buffer = NULL;
        rx->transport.adc.raw_buffer_size = 0U;
        rx->transport.adc.parsed_capacity = 0U;
        return ESPIO_NO_MEM;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize the ADC continuous backend.
 */
static int32_t mic_rx_init_adc_backend(espio_audio_mic_rx_t* rx,
                                       const espio_audio_mic_adc_config_t* adc_config,
                                       const espio_audio_mic_rx_config_t* config) {
    if (!rx || !adc_config || !config) {
        return ESPIO_INVALID_ARG;
    }

    if (adc_config->format.channel_mode != ESPIO_AUDIO_CHANNEL_MODE_MONO) {
        ESPIO_LOGE(TAG, "ADC microphones only support mono capture");
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    adc_digi_convert_mode_t conv_mode = ADC_CONV_SINGLE_UNIT_1;
    if (mic_rx_map_adc_unit(adc_config->adc_unit, &conv_mode) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "invalid ADC unit");
        return ESPIO_INVALID_ARG;
    }

    adc_atten_t atten = ADC_ATTEN_DB_12;
    if (mic_rx_map_adc_atten(adc_config->attenuation_db, &atten) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "invalid ADC attenuation selector");
        return ESPIO_INVALID_ARG;
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = adc_config->max_store_buffer_size > 0U ? adc_config->max_store_buffer_size : (uint32_t)MIC_ADC_DEFAULT_STORE_SIZE,
        .conv_frame_size = adc_config->frame_size > 0U ? adc_config->frame_size : (uint32_t)MIC_ADC_DEFAULT_FRAME_SIZE,
    };

    esp_err_t err = adc_continuous_new_handle(&handle_cfg, &rx->transport.adc.handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to allocate ADC continuous handle: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_DRIVER_FAILED;
    }

    adc_digi_pattern_config_t pattern = {
        .atten = (uint8_t)atten,
        .channel = (uint8_t)adc_config->adc_channel,
        .unit = (uint8_t)(adc_config->adc_unit - 1),
        .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
    };

    adc_continuous_config_t adc_cfg = {
        .pattern_num = 1U,
        .adc_pattern = &pattern,
        .sample_freq_hz = adc_config->sample_rate_hz,
        .conv_mode = conv_mode,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };

    err = adc_continuous_config(rx->transport.adc.handle, &adc_cfg);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to configure ADC continuous mode: %s", esp_err_to_name(err));
        (void)adc_continuous_deinit(rx->transport.adc.handle);
        rx->transport.adc.handle = NULL;
        return ESPIO_AUDIO_ERR_DRIVER_FAILED;
    }

    size_t requested_buffer = config->buffer_size > 0U ? config->buffer_size : handle_cfg.conv_frame_size;
    if (requested_buffer < handle_cfg.conv_frame_size) {
        requested_buffer = handle_cfg.conv_frame_size;
    }

    int32_t result = mic_rx_allocate_adc_buffers(rx, requested_buffer);
    if (result != ESPIO_AUDIO_OK) {
        (void)adc_continuous_deinit(rx->transport.adc.handle);
        rx->transport.adc.handle = NULL;
        return result;
    }

    rx->transport.adc.adc_unit = adc_config->adc_unit;
    rx->transport.adc.adc_channel = adc_config->adc_channel;
    rx->backend = MIC_RX_BACKEND_ADC;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize one I2S-family backend.
 */
static int32_t mic_rx_init_i2s_backend(espio_audio_mic_rx_t* rx,
                                       const espio_audio_mic_transport_config_t* transport_config) {
    if (!rx || !transport_config) {
        return ESPIO_INVALID_ARG;
    }

    int32_t result = ESPIO_UNSUPPORTED;
    switch (transport_config->interface_type) {
        case ESPIO_AUDIO_MIC_INTERFACE_I2S:
            result = audio_i2s_hal_init_rx_with_slot_mask(&rx->transport.i2s,
                                                          &transport_config->config.i2s.format,
                                                          &transport_config->config.i2s.pins,
                                                          transport_config->config.i2s.controller_id,
                                                          transport_config->config.i2s.mclk_policy,
                                                          transport_config->config.i2s.capture_right_slot ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT);
            break;
        case ESPIO_AUDIO_MIC_INTERFACE_PDM:
            result = audio_i2s_hal_init_pdm_rx(&rx->transport.i2s,
                                               &transport_config->config.pdm.format,
                                               transport_config->config.pdm.clk_pin,
                                               transport_config->config.pdm.data_pin,
                                               transport_config->config.pdm.controller_id,
                                               transport_config->config.pdm.hardware_high_pass_enabled,
                                               transport_config->config.pdm.hardware_high_pass_cutoff_hz,
                                               transport_config->config.pdm.hardware_amplify);
            break;
        case ESPIO_AUDIO_MIC_INTERFACE_TDM:
            result = audio_i2s_hal_init_tdm_rx(&rx->transport.i2s,
                                               &transport_config->config.tdm.format,
                                               &transport_config->config.tdm.pins,
                                               transport_config->config.tdm.controller_id,
                                               transport_config->config.tdm.mclk_policy,
                                               transport_config->config.tdm.channel_count);
            break;
        default:
            return ESPIO_UNSUPPORTED;
    }

    if (result == ESPIO_AUDIO_OK) {
        rx->backend = MIC_RX_BACKEND_I2S;
    }
    return result;
}

/**
 * @brief Decode ADC samples into the caller buffer.
 */
static int32_t mic_rx_read_adc_backend(espio_audio_mic_rx_t* rx, void* samples, size_t size, size_t* bytes_read, uint32_t timeout_ms) {
    if (!rx || !samples || size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    uint32_t raw_bytes_read = 0U;
    esp_err_t err = adc_continuous_read(rx->transport.adc.handle,
                                        rx->transport.adc.raw_buffer,
                                        (uint32_t)rx->transport.adc.raw_buffer_size,
                                        &raw_bytes_read,
                                        timeout_ms);
    if (err == ESP_ERR_TIMEOUT) {
        if (bytes_read) {
            *bytes_read = 0U;
        }
        return ESPIO_AUDIO_OK;
    }
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to read ADC samples: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_DRIVER_FAILED;
    }

    uint32_t parsed_count = 0U;
    err = adc_continuous_parse_data(rx->transport.adc.handle,
                                    rx->transport.adc.raw_buffer,
                                    raw_bytes_read,
                                    rx->transport.adc.parsed_buffer,
                                    &parsed_count);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to parse ADC samples: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_DRIVER_FAILED;
    }

    size_t sample_size = mic_rx_sample_size_bytes(rx->format.bit_width);
    if (sample_size == 0U) {
        return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }

    if (size < ((size_t)parsed_count * sample_size)) {
        ESPIO_LOGE(TAG, "destination buffer is too small for ADC samples");
        return ESPIO_INVALID_ARG;
    }

    uint8_t* output = (uint8_t*)samples;
    size_t produced = 0U;
    for (uint32_t sample_index = 0U; sample_index < parsed_count; sample_index++) {
        const adc_continuous_data_t* parsed = &rx->transport.adc.parsed_buffer[sample_index];
        if (!parsed->valid) {
            continue;
        }
        if ((int)parsed->unit != (rx->transport.adc.adc_unit - 1) || (int)parsed->channel != rx->transport.adc.adc_channel) {
            continue;
        }

        int32_t centered_sample = (int32_t)parsed->raw_data - 2048;
        int32_t widened_sample = centered_sample << 4;
        switch (rx->format.bit_width) {
            case ESPIO_AUDIO_BIT_WIDTH_16:
                mic_rx_store_sample(&output[produced], rx->format.bit_width, (float)widened_sample);
                break;
            case ESPIO_AUDIO_BIT_WIDTH_24:
                mic_rx_store_sample(&output[produced], rx->format.bit_width, (float)(widened_sample << 8));
                break;
            case ESPIO_AUDIO_BIT_WIDTH_32:
                mic_rx_store_sample(&output[produced], rx->format.bit_width, (float)(widened_sample << 16));
                break;
            default:
                return ESPIO_AUDIO_ERR_BIT_WIDTH;
        }

        produced += sample_size;
    }

    if (bytes_read) {
        *bytes_read = produced;
    }
    if (produced == 0U) {
        return ESPIO_AUDIO_OK;
    }

    return mic_rx_process_samples(rx, samples, produced);
}

/**
 * @brief Deinitialize one ADC backend.
 */
static void mic_rx_deinit_adc_backend(espio_audio_mic_rx_t* rx) {
    if (!rx) {
        return;
    }

    if (rx->started && rx->transport.adc.handle) {
        (void)adc_continuous_stop(rx->transport.adc.handle);
    }
    if (rx->transport.adc.handle) {
        (void)adc_continuous_deinit(rx->transport.adc.handle);
        rx->transport.adc.handle = NULL;
    }
    if (rx->transport.adc.raw_buffer) {
        heap_caps_free(rx->transport.adc.raw_buffer);
        rx->transport.adc.raw_buffer = NULL;
    }
    if (rx->transport.adc.parsed_buffer) {
        heap_caps_free(rx->transport.adc.parsed_buffer);
        rx->transport.adc.parsed_buffer = NULL;
    }
}

/**
 * @brief Create and configure a microphone RX helper.
 */
espio_audio_mic_rx_t* espio_audio_mic_rx_create(espio_audio_mic_t* mic, const espio_audio_mic_rx_config_t* config) {
    if (!mic || !config) {
        return NULL;
    }

    const espio_audio_mic_transport_config_t* transport_config = &mic->transport_config;
    const espio_audio_format_t* format = NULL;
    switch (transport_config->interface_type) {
        case ESPIO_AUDIO_MIC_INTERFACE_I2S:
            format = &transport_config->config.i2s.format;
            break;
        case ESPIO_AUDIO_MIC_INTERFACE_PDM:
            format = &transport_config->config.pdm.format;
            break;
        case ESPIO_AUDIO_MIC_INTERFACE_ADC:
            format = &transport_config->config.adc.format;
            break;
        case ESPIO_AUDIO_MIC_INTERFACE_TDM:
            format = &transport_config->config.tdm.format;
            break;
        default:
            ESPIO_LOGE(TAG, "transport is not implemented for %s", mic->driver->name);
            return NULL;
    }

    if (mic_rx_validate_format(&mic->caps, format, transport_config->interface_type) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "requested format is unsupported by microphone capabilities");
        return NULL;
    }

    espio_audio_mic_rx_t* rx = heap_caps_calloc(1, sizeof(espio_audio_mic_rx_t), MALLOC_CAP_DEFAULT);
    if (!rx) {
        ESPIO_LOGE(TAG, "failed to allocate microphone RX transport");
        return NULL;
    }

    rx->mic = mic;
    rx->format = *format;
    rx->active_channel_count = mic_rx_resolve_channel_count(mic);
    rx->default_timeout_ms = config->read_timeout_ms;
    rx->agc_target_level_db = -12;
    rx->agc_gain_linear = 1.0f;
    rx->extra_gain_linear = 1.0f;

    int32_t result = ESPIO_AUDIO_OK;
    switch (transport_config->interface_type) {
        case ESPIO_AUDIO_MIC_INTERFACE_I2S:
        case ESPIO_AUDIO_MIC_INTERFACE_PDM:
        case ESPIO_AUDIO_MIC_INTERFACE_TDM:
            result = mic_rx_init_i2s_backend(rx, transport_config);
            break;
        case ESPIO_AUDIO_MIC_INTERFACE_ADC:
            result = mic_rx_init_adc_backend(rx, &transport_config->config.adc, config);
            break;
        default:
            result = ESPIO_UNSUPPORTED;
            break;
    }

    if (result != ESPIO_AUDIO_OK) {
        espio_audio_mic_rx_destroy(rx);
        return NULL;
    }

    ESPIO_LOGI(TAG, "microphone RX transport created");
    return rx;
}

/**
 * @brief Destroy a microphone RX helper.
 */
void espio_audio_mic_rx_destroy(espio_audio_mic_rx_t* rx) {
    if (!rx) {
        return;
    }

    if (rx->backend == MIC_RX_BACKEND_I2S) {
        audio_i2s_hal_deinit(&rx->transport.i2s);
    } else if (rx->backend == MIC_RX_BACKEND_ADC) {
        mic_rx_deinit_adc_backend(rx);
    }

    heap_caps_free(rx);
}

/**
 * @brief Start the RX transport path.
 */
int32_t espio_audio_mic_rx_start(espio_audio_mic_rx_t* rx) {
    if (!rx) {
        return ESPIO_INVALID_ARG;
    }

    int32_t result = ESPIO_UNSUPPORTED;
    if (rx->backend == MIC_RX_BACKEND_I2S) {
        result = audio_i2s_hal_start_rx(&rx->transport.i2s);
    } else if (rx->backend == MIC_RX_BACKEND_ADC) {
        esp_err_t err = adc_continuous_start(rx->transport.adc.handle);
        result = err == ESP_OK ? ESPIO_AUDIO_OK : ESPIO_AUDIO_ERR_DRIVER_FAILED;
        if (err != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to start ADC capture: %s", esp_err_to_name(err));
        }
    }

    if (result != ESPIO_AUDIO_OK) {
        return result;
    }

    rx->started = true;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Stop the RX transport path.
 */
int32_t espio_audio_mic_rx_stop(espio_audio_mic_rx_t* rx) {
    if (!rx) {
        return ESPIO_INVALID_ARG;
    }

    int32_t result = ESPIO_UNSUPPORTED;
    if (rx->backend == MIC_RX_BACKEND_I2S) {
        result = audio_i2s_hal_stop_rx(&rx->transport.i2s);
    } else if (rx->backend == MIC_RX_BACKEND_ADC) {
        esp_err_t err = adc_continuous_stop(rx->transport.adc.handle);
        result = err == ESP_OK ? ESPIO_AUDIO_OK : ESPIO_AUDIO_ERR_DRIVER_FAILED;
        if (err != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to stop ADC capture: %s", esp_err_to_name(err));
        }
    }

    if (result != ESPIO_AUDIO_OK) {
        return result;
    }

    rx->started = false;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Read PCM data from the RX transport.
 */
int32_t espio_audio_mic_rx_read(espio_audio_mic_rx_t* rx, void* samples, size_t size, size_t* bytes_read, uint32_t timeout_ms) {
    if (!rx || !samples || size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    if (!rx->started) {
        ESPIO_LOGE(TAG, "microphone RX transport is not started");
        return ESPIO_AUDIO_ERR_NOT_STARTED;
    }

    uint32_t effective_timeout_ms = timeout_ms == 0U ? rx->default_timeout_ms : timeout_ms;
    if (rx->backend == MIC_RX_BACKEND_I2S) {
        size_t local_bytes_read = 0U;
        int32_t result = audio_i2s_hal_read(&rx->transport.i2s, samples, size, &local_bytes_read, effective_timeout_ms);
        if (bytes_read) {
            *bytes_read = local_bytes_read;
        }
        if (result != ESPIO_AUDIO_OK || local_bytes_read == 0U) {
            return result;
        }

        return mic_rx_process_samples(rx, samples, local_bytes_read);
    }
    if (rx->backend == MIC_RX_BACKEND_ADC) {
        return mic_rx_read_adc_backend(rx, samples, size, bytes_read, effective_timeout_ms);
    }

    return ESPIO_UNSUPPORTED;
}

/**
 * @brief Enable or disable the software high-pass filter.
 */
int32_t espio_audio_mic_rx_enable_high_pass(espio_audio_mic_rx_t* rx, bool enable, float cutoff_hz) {
    if (!rx || (enable && cutoff_hz <= 0.0f)) {
        return ESPIO_INVALID_ARG;
    }

    rx->high_pass_enabled = enable;
    rx->high_pass_cutoff_hz = enable ? cutoff_hz : 0.0f;
    for (size_t i = 0; i < ESPIO_AUDIO_MIC_INTERNAL_MAX_CHANNELS; i++) {
        rx->high_pass_prev_input[i] = 0.0f;
        rx->high_pass_prev_output[i] = 0.0f;
    }
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Set an additional software gain multiplier for captured samples.
 */
int32_t espio_audio_mic_rx_set_software_gain(espio_audio_mic_rx_t* rx, float gain_linear) {
    if (!rx || gain_linear <= 0.0f) {
        return ESPIO_INVALID_ARG;
    }

    rx->extra_gain_linear = gain_linear;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Enable or disable software AGC.
 */
int32_t espio_audio_mic_rx_enable_agc(espio_audio_mic_rx_t* rx, bool enable) {
    if (!rx) {
        return ESPIO_INVALID_ARG;
    }

    rx->agc_enabled = enable;
    rx->agc_gain_linear = 1.0f;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Set the AGC target level.
 */
int32_t espio_audio_mic_rx_set_agc_target(espio_audio_mic_rx_t* rx, int8_t target_level_db) {
    if (!rx || target_level_db > 0 || target_level_db < -96) {
        return ESPIO_INVALID_ARG;
    }

    rx->agc_target_level_db = target_level_db;
    return ESPIO_AUDIO_OK;
}
