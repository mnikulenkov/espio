/**
 * @file audio_ctrl_gpio.h
 * @brief Private GPIO control helpers.
 */

#ifndef AUDIO_CTRL_GPIO_H
#define AUDIO_CTRL_GPIO_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Private GPIO-backed control pin state.
 */
typedef struct {
    int pin;             ///< Public GPIO number or AUDIO_PIN_UNUSED.
    bool active_high;    ///< True when a high level asserts the control.
    bool configured;     ///< True when the pin has been configured locally.
} audio_ctrl_gpio_t;

/**
 * @brief Initialize a GPIO control pin when one is configured.
 * @param ctrl Control-pin state.
 * @param pin GPIO number or AUDIO_PIN_UNUSED.
 * @param active_high True when a high level asserts the control.
 * @param asserted Initial asserted state.
 * @return AUDIO_OK on success, error code on failure.
 */
int32_t audio_ctrl_gpio_init(audio_ctrl_gpio_t* ctrl, int pin, bool active_high, bool asserted);

/**
 * @brief Deinitialize a GPIO control pin.
 * @param ctrl Control-pin state.
 */
void audio_ctrl_gpio_deinit(audio_ctrl_gpio_t* ctrl);

/**
 * @brief Set a GPIO control pin state.
 * @param ctrl Control-pin state.
 * @param asserted Requested logical state.
 * @return AUDIO_OK on success, error code on failure.
 */
int32_t audio_ctrl_gpio_set(audio_ctrl_gpio_t* ctrl, bool asserted);

#endif /* AUDIO_CTRL_GPIO_H */
