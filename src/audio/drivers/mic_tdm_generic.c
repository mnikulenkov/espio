/**
 * @file mic_tdm_generic.c
 * @brief Generic TDM microphone driver.
 */

#include "espio/drivers/mic_tdm_generic.h"
#include "esp_heap_caps.h"
#include "espio/audio_err.h"

/**
 * @brief Query capabilities for a generic TDM microphone configuration.
 */
static int32_t mic_tdm_generic_query_caps(const void* config, espio_audio_mic_caps_t* caps) {
    if (!config || !caps) {
        return ESPIO_INVALID_ARG;
    }

    static const espio_audio_bit_width_t supported_bit_widths[] = {
        ESPIO_AUDIO_BIT_WIDTH_16,
        ESPIO_AUDIO_BIT_WIDTH_24,
        ESPIO_AUDIO_BIT_WIDTH_32,
    };

    *caps = (espio_audio_mic_caps_t) {
        .interface_type = ESPIO_AUDIO_MIC_INTERFACE_TDM,
        .gain_type = ESPIO_AUDIO_MIC_GAIN_TYPE_NONE,
        .min_gain_db = -24,
        .max_gain_db = 24,
        .default_gain_db = 0,
        .min_sample_rate_hz = 16000U,
        .max_sample_rate_hz = 96000U,
        .supported_bit_widths = supported_bit_widths,
        .supported_bit_width_count = sizeof(supported_bit_widths) / sizeof(supported_bit_widths[0]),
        .supports_stereo = true,
        .supports_high_pass_filter = false,
        .supports_agc = false,
        .supports_sleep_mode = false,
    };

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Normalize the transport configuration for the generic RX layer.
 */
static int32_t mic_tdm_generic_get_transport_config(const void* config, espio_audio_mic_transport_config_t* transport_config) {
    if (!config || !transport_config) {
        return ESPIO_INVALID_ARG;
    }

    const espio_audio_mic_tdm_generic_config_t* driver_config = (const espio_audio_mic_tdm_generic_config_t*)config;
    *transport_config = (espio_audio_mic_transport_config_t) {
        .interface_type = ESPIO_AUDIO_MIC_INTERFACE_TDM,
        .config.tdm = driver_config->tdm_config,
    };

    transport_config->config.tdm.pins.dout_pin = ESPIO_AUDIO_PIN_UNUSED;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize generic TDM driver state.
 */
static void* mic_tdm_generic_init(const void* config) {
    if (!config) {
        return NULL;
    }

    return heap_caps_calloc(1, 1U, MALLOC_CAP_DEFAULT);
}

/**
 * @brief Deinitialize generic TDM driver state.
 */
static void mic_tdm_generic_deinit(void* ctx) {
    if (!ctx) {
        return;
    }

    heap_caps_free(ctx);
}

/**
 * @brief Generic TDM driver descriptor.
 */
static const espio_audio_mic_driver_t s_mic_tdm_generic_driver = {
    .name = "generic_tdm",
    .interface_type = ESPIO_AUDIO_MIC_INTERFACE_TDM,
    .query_caps = mic_tdm_generic_query_caps,
    .get_transport_config = mic_tdm_generic_get_transport_config,
    .init = mic_tdm_generic_init,
    .deinit = mic_tdm_generic_deinit,
    .set_power = NULL,
    .set_gain = NULL,
    .get_gain = NULL,
    .configure_filters = NULL,
};

/**
 * @brief Get the generic TDM microphone driver instance.
 */
const espio_audio_mic_driver_t* mic_driver_tdm_generic(void) {
    return &s_mic_tdm_generic_driver;
}
