/**
 * @file button_gpio.h
 * @brief Direct GPIO provider for the generic button abstraction API.
 */

#ifndef ESPIO_BUTTON_GPIO_H
#define ESPIO_BUTTON_GPIO_H

#include "espio/button.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Public GPIO pull configuration used by the GPIO provider.
 */
typedef enum {
    ESPIO_BUTTON_GPIO_PULL_NONE = 0, ///< Do not enable internal pull resistors.
    ESPIO_BUTTON_GPIO_PULL_UP = 1,   ///< Enable the internal pull-up resistor.
    ESPIO_BUTTON_GPIO_PULL_DOWN = 2  ///< Enable the internal pull-down resistor.
} espio_button_gpio_pull_mode_t;

/**
 * @brief One GPIO-backed physical channel.
 */
typedef struct {
    espio_button_channel_id_t channel_id;  ///< Stable provider-local channel identifier.
    int pin;                               ///< GPIO number connected to the physical button.
    bool active_high;                      ///< True when logical active corresponds to GPIO high.
    bool enable_interrupt;                 ///< True when this channel should wake the manager via GPIO interrupt.
    espio_button_gpio_pull_mode_t pull_mode; ///< Internal pull resistor mode applied during initialization.
} espio_button_gpio_channel_config_t;

/**
 * @brief Top-level GPIO provider configuration.
 */
typedef struct {
    const espio_button_gpio_channel_config_t* channels; ///< Channel definitions owned by the caller during creation.
    size_t channel_count;                         ///< Number of GPIO channel definitions.
} espio_button_gpio_config_t;

/**
 * @brief Get the GPIO provider driver descriptor.
 * @return Immutable GPIO driver descriptor.
 */
const espio_button_driver_t* espio_button_driver_gpio(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_BUTTON_GPIO_H */
