/**
 * @file audio_ctrl_gpio.c
 * @brief GPIO control helpers for amplifier drivers.
 */

#include "audio_ctrl_gpio.h"
#include "espio/audio_amp.h"
#include "driver/gpio.h"
#include "espio/log.h"
#include <string.h>

static const char* TAG = "audio_gpio";

/**
 * @brief Convert a logical asserted state into a GPIO level.
 */
static int audio_ctrl_gpio_level(const audio_ctrl_gpio_t* ctrl, bool asserted) {
    return asserted == ctrl->active_high ? 1 : 0;
}

/**
 * @brief Initialize a GPIO control pin when one is configured.
 */
int32_t audio_ctrl_gpio_init(audio_ctrl_gpio_t* ctrl, int pin, bool active_high, bool asserted) {
    if (!ctrl) {
        return ESPIO_INVALID_ARG;
    }

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pin = pin;
    ctrl->active_high = active_high;

    if (pin == ESPIO_AUDIO_PIN_UNUSED) {
        return ESPIO_AUDIO_OK;
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to configure GPIO %d", pin);
        return ESPIO_AUDIO_ERR_GPIO;
    }

    ctrl->configured = true;
    return audio_ctrl_gpio_set(ctrl, asserted);
}

/**
 * @brief Deinitialize a GPIO control pin.
 */
void audio_ctrl_gpio_deinit(audio_ctrl_gpio_t* ctrl) {
    if (!ctrl || !ctrl->configured) {
        return;
    }

    (void)gpio_reset_pin((gpio_num_t)ctrl->pin);
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->pin = ESPIO_AUDIO_PIN_UNUSED;
}

/**
 * @brief Set a GPIO control pin state.
 */
int32_t audio_ctrl_gpio_set(audio_ctrl_gpio_t* ctrl, bool asserted) {
    if (!ctrl) {
        return ESPIO_INVALID_ARG;
    }

    if (ctrl->pin == ESPIO_AUDIO_PIN_UNUSED) {
        return ESPIO_AUDIO_OK;
    }

    esp_err_t err = gpio_set_level((gpio_num_t)ctrl->pin, audio_ctrl_gpio_level(ctrl, asserted));
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to drive GPIO %d", ctrl->pin);
        return ESPIO_AUDIO_ERR_GPIO;
    }

    return ESPIO_AUDIO_OK;
}
