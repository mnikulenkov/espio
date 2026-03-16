/**
 * @file amp_max98357a.c
 * @brief MAX98357A digital-input amplifier driver.
 */

#include "espio/drivers/amp_max98357a.h"
#include "audio_ctrl_gpio.h"
#include "esp_heap_caps.h"
#include "espio/log.h"

static const char* TAG = "max98357a";

/**
 * @brief MAX98357A driver state.
 */
typedef struct {
    audio_ctrl_gpio_t sd_mode_gpio; ///< Shared shutdown / mute control pin.
    bool power_on;                  ///< Last requested power state.
    bool muted;                     ///< Last requested mute state.
} espio_audio_max98357a_t;

/**
 * @brief Apply the effective SD_MODE level.
 *
 * The MAX98357A uses one pin for both shutdown and mute-like behavior, so the
 * driver keeps explicit power and mute state and derives one output level.
 *
 * @param amp Driver state.
 * @return AUDIO_OK on success, or error code on failure.
 */
static int32_t espio_audio_max98357a_apply_state(espio_audio_max98357a_t* amp) {
    if (!amp) {
        return ESPIO_INVALID_ARG;
    }

    return audio_ctrl_gpio_set(&amp->sd_mode_gpio, amp->power_on && !amp->muted);
}

/**
 * @brief Query capabilities for a MAX98357A configuration.
 */
static int32_t espio_audio_max98357a_query_caps(const void* config, espio_audio_caps_t* caps) {
    if (!config || !caps) {
        return ESPIO_INVALID_ARG;
    }

    static const espio_audio_bit_width_t supported_bit_widths[] = {
        ESPIO_AUDIO_BIT_WIDTH_16,
        ESPIO_AUDIO_BIT_WIDTH_24,
        ESPIO_AUDIO_BIT_WIDTH_32,
    };
    static const espio_audio_serial_mode_t supported_serial_modes[] = {
        ESPIO_AUDIO_SERIAL_MODE_I2S_PHILIPS,
    };

    const espio_audio_max98357a_config_t* driver_config = (const espio_audio_max98357a_config_t*)config;
    *caps = (espio_audio_caps_t) {
        .input_class = ESPIO_AUDIO_INPUT_CLASS_DIGITAL,
        .control_bus = driver_config->sd_mode_pin == ESPIO_AUDIO_PIN_UNUSED ? ESPIO_AUDIO_CONTROL_BUS_NONE : ESPIO_AUDIO_CONTROL_BUS_GPIO,
        .power_control = driver_config->sd_mode_pin == ESPIO_AUDIO_PIN_UNUSED ? ESPIO_AUDIO_POWER_CONTROL_NONE : ESPIO_AUDIO_POWER_CONTROL_GPIO,
        .mclk_policy = ESPIO_AUDIO_MCLK_POLICY_DISABLED,
        .supports_mute = driver_config->sd_mode_pin != ESPIO_AUDIO_PIN_UNUSED,
        .supports_hardware_volume = false,
        .supports_software_volume = false,
        .supports_dsp = false,
        .min_sample_rate_hz = 8000U,
        .max_sample_rate_hz = 96000U,
        .supported_bit_widths = supported_bit_widths,
        .supported_bit_width_count = sizeof(supported_bit_widths) / sizeof(supported_bit_widths[0]),
        .supported_serial_modes = supported_serial_modes,
        .supported_serial_mode_count = sizeof(supported_serial_modes) / sizeof(supported_serial_modes[0]),
    };

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize MAX98357A driver state.
 */
static void* espio_audio_max98357a_init(const void* config) {
    if (!config) {
        return NULL;
    }

    const espio_audio_max98357a_config_t* driver_config = (const espio_audio_max98357a_config_t*)config;

    espio_audio_max98357a_t* amp = heap_caps_calloc(1, sizeof(espio_audio_max98357a_t), MALLOC_CAP_DEFAULT);
    if (!amp) {
        ESPIO_LOGE(TAG, "failed to allocate driver state");
        return NULL;
    }

    amp->power_on = true;
    amp->muted = false;

    if (audio_ctrl_gpio_init(&amp->sd_mode_gpio,
                             driver_config->sd_mode_pin,
                             driver_config->sd_mode_active_high,
                             amp->power_on) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to initialize shutdown pin");
        heap_caps_free(amp);
        return NULL;
    }

    ESPIO_LOGI(TAG, "MAX98357A driver initialized");
    return amp;
}

/**
 * @brief Deinitialize MAX98357A driver state.
 */
static void espio_audio_max98357a_deinit(void* ctx) {
    espio_audio_max98357a_t* amp = (espio_audio_max98357a_t*)ctx;
    if (!amp) {
        return;
    }

    audio_ctrl_gpio_deinit(&amp->sd_mode_gpio);
    heap_caps_free(amp);
}

/**
 * @brief Set MAX98357A power state.
 */
static int32_t espio_audio_max98357a_set_power(void* ctx, bool on) {
    espio_audio_max98357a_t* amp = (espio_audio_max98357a_t*)ctx;
    if (!amp) {
        return ESPIO_INVALID_ARG;
    }

    amp->power_on = on;
    return espio_audio_max98357a_apply_state(amp);
}

/**
 * @brief Set MAX98357A mute state.
 */
static int32_t espio_audio_max98357a_set_mute(void* ctx, bool mute) {
    espio_audio_max98357a_t* amp = (espio_audio_max98357a_t*)ctx;
    if (!amp) {
        return ESPIO_INVALID_ARG;
    }

    amp->muted = mute;
    return espio_audio_max98357a_apply_state(amp);
}

/**
 * @brief MAX98357A driver descriptor.
 */
static const espio_audio_driver_t espio_audio_max98357a_driver = {
    .name = "MAX98357A",
    .query_caps = espio_audio_max98357a_query_caps,
    .init = espio_audio_max98357a_init,
    .deinit = espio_audio_max98357a_deinit,
    .set_power = espio_audio_max98357a_set_power,
    .set_mute = espio_audio_max98357a_set_mute,
    .set_volume = NULL,
};

/**
 * @brief Get the MAX98357A driver instance.
 */
const espio_audio_driver_t* espio_audio_driver_max98357a(void) {
    return &espio_audio_max98357a_driver;
}
