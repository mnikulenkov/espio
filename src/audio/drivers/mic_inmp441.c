/**
 * @file mic_inmp441.c
 * @brief INMP441 digital microphone driver.
 */

#include "espio/drivers/mic_inmp441.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "espio/audio_err.h"
#include "espio/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "mic_inmp441";
static const uint32_t MIC_INMP441_WAKE_DELAY_MS = 45U;

/**
 * @brief INMP441 driver state.
 */
typedef struct {
    int lr_pin;                  ///< Optional L/R select GPIO.
    int chipen_pin;              ///< Optional CHIPEN GPIO.
    bool left_channel;           ///< True when the left slot is selected.
    bool left_channel_level_high;///< True when a high level selects the left slot.
    bool chipen_level_high;      ///< True when a high level enables the microphone.
    bool powered;                ///< True when the microphone is enabled.
} mic_inmp441_t;

/**
 * @brief Validate one INMP441 configuration before the driver exposes it.
 */
static int32_t mic_inmp441_validate_config(const espio_audio_mic_inmp441_config_t* config) {
    if (!config) {
        return ESPIO_INVALID_ARG;
    }
    if (config->i2s_config.format.serial_mode != ESPIO_AUDIO_SERIAL_MODE_I2S_PHILIPS) {
        return ESPIO_AUDIO_ERR_INVALID_FORMAT;
    }
    if (config->i2s_config.format.channel_mode != ESPIO_AUDIO_CHANNEL_MODE_MONO) {
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }
    if (config->i2s_config.pins.bclk_pin < 0 || config->i2s_config.pins.ws_pin < 0 || config->i2s_config.pins.din_pin < 0) {
        return ESPIO_INVALID_ARG;
    }
    if (config->lr_pin >= 0 && config->chipen_pin >= 0 && config->lr_pin == config->chipen_pin) {
        return ESPIO_INVALID_ARG;
    }

    switch (config->i2s_config.format.bit_width) {
        case ESPIO_AUDIO_BIT_WIDTH_16:
        case ESPIO_AUDIO_BIT_WIDTH_24:
        case ESPIO_AUDIO_BIT_WIDTH_32:
            return ESPIO_AUDIO_OK;
        default:
            return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }
}

/**
 * @brief Drive one optional GPIO-controlled function pin.
 */
static int32_t mic_inmp441_set_optional_pin(int pin, bool level_high) {
    if (pin == ESPIO_AUDIO_PIN_UNUSED) {
        return ESPIO_AUDIO_OK;
    }

    esp_err_t err = gpio_set_level((gpio_num_t)pin, level_high ? 1 : 0);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to drive GPIO %d: %s", pin, esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_GPIO;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Configure one optional output pin used by the microphone.
 */
static int32_t mic_inmp441_configure_output_pin(int pin) {
    if (pin == ESPIO_AUDIO_PIN_UNUSED) {
        return ESPIO_AUDIO_OK;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to configure GPIO %d: %s", pin, esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_GPIO;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Query capabilities for an INMP441 configuration.
 */
static int32_t mic_inmp441_query_caps(const void* config, espio_audio_mic_caps_t* caps) {
    if (!config || !caps) {
        return ESPIO_INVALID_ARG;
    }

    const espio_audio_mic_inmp441_config_t* driver_config = (const espio_audio_mic_inmp441_config_t*)config;
    int32_t result = mic_inmp441_validate_config(driver_config);
    if (result != ESPIO_AUDIO_OK) {
        return result;
    }

    static const espio_audio_bit_width_t supported_bit_widths[] = {
        ESPIO_AUDIO_BIT_WIDTH_16,
        ESPIO_AUDIO_BIT_WIDTH_24,
        ESPIO_AUDIO_BIT_WIDTH_32,
    };

    *caps = (espio_audio_mic_caps_t) {
        .interface_type = ESPIO_AUDIO_MIC_INTERFACE_I2S,
        .gain_type = ESPIO_AUDIO_MIC_GAIN_TYPE_NONE,
        .min_gain_db = -24,
        .max_gain_db = 24,
        .default_gain_db = 0,
        .min_sample_rate_hz = 16000U,
        .max_sample_rate_hz = 48000U,
        .supported_bit_widths = supported_bit_widths,
        .supported_bit_width_count = sizeof(supported_bit_widths) / sizeof(supported_bit_widths[0]),
        .supports_stereo = false,
        .supports_high_pass_filter = false,
        .supports_agc = false,
        .supports_sleep_mode = driver_config->chipen_pin != ESPIO_AUDIO_PIN_UNUSED,
    };

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Normalize the transport configuration for the generic RX layer.
 */
static int32_t mic_inmp441_get_transport_config(const void* config, espio_audio_mic_transport_config_t* transport_config) {
    if (!config || !transport_config) {
        return ESPIO_INVALID_ARG;
    }

    const espio_audio_mic_inmp441_config_t* driver_config = (const espio_audio_mic_inmp441_config_t*)config;
    int32_t result = mic_inmp441_validate_config(driver_config);
    if (result != ESPIO_AUDIO_OK) {
        return result;
    }

    *transport_config = (espio_audio_mic_transport_config_t) {
        .interface_type = ESPIO_AUDIO_MIC_INTERFACE_I2S,
        .config.i2s = driver_config->i2s_config,
    };

    transport_config->config.i2s.pins.dout_pin = ESPIO_AUDIO_PIN_UNUSED;
    transport_config->config.i2s.capture_right_slot = !driver_config->left_channel;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Set the current power state through CHIPEN when it is wired.
 */
static int32_t mic_inmp441_set_power(void* ctx, bool on) {
    mic_inmp441_t* mic = (mic_inmp441_t*)ctx;
    if (!mic) {
        return ESPIO_INVALID_ARG;
    }
    if (mic->chipen_pin == ESPIO_AUDIO_PIN_UNUSED) {
        return ESPIO_UNSUPPORTED;
    }

    int32_t result = mic_inmp441_set_optional_pin(mic->chipen_pin, on ? mic->chipen_level_high : !mic->chipen_level_high);
    if (result != ESPIO_AUDIO_OK) {
        return result;
    }

    if (on && !mic->powered) {
        vTaskDelay(pdMS_TO_TICKS(MIC_INMP441_WAKE_DELAY_MS));
    }

    mic->powered = on;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize INMP441 driver state.
 */
static void* mic_inmp441_init(const void* config) {
    if (!config) {
        return NULL;
    }

    const espio_audio_mic_inmp441_config_t* driver_config = (const espio_audio_mic_inmp441_config_t*)config;
    if (mic_inmp441_validate_config(driver_config) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "invalid INMP441 configuration");
        return NULL;
    }

    mic_inmp441_t* mic = heap_caps_calloc(1, sizeof(mic_inmp441_t), MALLOC_CAP_DEFAULT);
    if (!mic) {
        ESPIO_LOGE(TAG, "failed to allocate driver state");
        return NULL;
    }

    mic->lr_pin = driver_config->lr_pin;
    mic->chipen_pin = driver_config->chipen_pin;
    mic->left_channel = driver_config->left_channel;
    mic->left_channel_level_high = driver_config->left_channel_level_high;
    mic->chipen_level_high = driver_config->chipen_level_high;
    mic->powered = true;

    if (mic_inmp441_configure_output_pin(mic->lr_pin) != ESPIO_AUDIO_OK ||
        mic_inmp441_configure_output_pin(mic->chipen_pin) != ESPIO_AUDIO_OK) {
        heap_caps_free(mic);
        return NULL;
    }

    if (mic->lr_pin != ESPIO_AUDIO_PIN_UNUSED) {
        int32_t result = mic_inmp441_set_optional_pin(mic->lr_pin,
                                                      mic->left_channel ? mic->left_channel_level_high : !mic->left_channel_level_high);
        if (result != ESPIO_AUDIO_OK) {
            heap_caps_free(mic);
            return NULL;
        }
    }

    if (mic->chipen_pin != ESPIO_AUDIO_PIN_UNUSED) {
        int32_t result = mic_inmp441_set_optional_pin(mic->chipen_pin, mic->chipen_level_high);
        if (result != ESPIO_AUDIO_OK) {
            heap_caps_free(mic);
            return NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(MIC_INMP441_WAKE_DELAY_MS));
    }

    ESPIO_LOGI(TAG, "INMP441 driver initialized");
    return mic;
}

/**
 * @brief Deinitialize INMP441 driver state.
 */
static void mic_inmp441_deinit(void* ctx) {
    mic_inmp441_t* mic = (mic_inmp441_t*)ctx;
    if (!mic) {
        return;
    }

    heap_caps_free(mic);
}

/**
 * @brief INMP441 driver descriptor.
 */
static const espio_audio_mic_driver_t s_mic_inmp441_driver = {
    .name = "INMP441",
    .interface_type = ESPIO_AUDIO_MIC_INTERFACE_I2S,
    .query_caps = mic_inmp441_query_caps,
    .get_transport_config = mic_inmp441_get_transport_config,
    .init = mic_inmp441_init,
    .deinit = mic_inmp441_deinit,
    .set_power = mic_inmp441_set_power,
    .set_gain = NULL,
    .get_gain = NULL,
    .configure_filters = NULL,
};

/**
 * @brief Get the INMP441 driver instance.
 */
const espio_audio_mic_driver_t* espio_audio_mic_driver_inmp441(void) {
    return &s_mic_inmp441_driver;
}
