/**
 * @file button_mcp23017.h
 * @brief MCP23017 I2C GPIO expander provider for the generic button API.
 */

#ifndef ESPIO_BUTTON_MCP23017_H
#define ESPIO_BUTTON_MCP23017_H

#include "espio/button.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCP23017 port selector used by channel definitions.
 */
typedef enum {
    ESPIO_BUTTON_MCP23017_PORT_A = 0, ///< Channel belongs to GPIOA.
    ESPIO_BUTTON_MCP23017_PORT_B = 1  ///< Channel belongs to GPIOB.
} espio_button_mcp23017_port_t;

/**
 * @brief One MCP23017-backed physical channel definition.
 */
typedef struct {
    espio_button_channel_id_t channel_id;  ///< Stable provider-local channel identifier.
    espio_button_mcp23017_port_t port;     ///< MCP23017 port selector.
    uint8_t pin;                           ///< Pin index inside the port in the 0-7 range.
    bool active_high;                      ///< True when logical active corresponds to a high register bit.
    bool enable_pullup;                    ///< True when the MCP23017 internal pull-up must be enabled.
} espio_button_mcp23017_channel_config_t;

/**
 * @brief Top-level MCP23017 provider configuration.
 */
typedef struct {
    int i2c_port;                                          ///< ESP32 I2C controller index used by the provider.
    int sda_pin;                                           ///< SDA GPIO used by the provider-owned I2C bus.
    int scl_pin;                                           ///< SCL GPIO used by the provider-owned I2C bus.
    uint8_t device_address;                                ///< MCP23017 7-bit address in the 0x20-0x27 range.
    uint32_t scl_speed_hz;                                 ///< I2C bus speed in Hertz.
    bool enable_bus_internal_pullups;                      ///< True when ESP32 internal SDA and SCL pull-ups should be enabled.
    int interrupt_pin;                                     ///< Optional ESP32 GPIO connected to the MCP23017 interrupt output, or ESPIO_BUTTON_PIN_UNUSED.
    bool interrupt_active_low;                             ///< True when the MCP23017 interrupt output is active low.
    uint32_t transaction_timeout_ms;                       ///< I2C transaction timeout in milliseconds.
    const espio_button_mcp23017_channel_config_t* channels; ///< MCP23017 channel definitions owned by the caller during creation.
    size_t channel_count;                                  ///< Number of MCP23017 channel definitions.
} espio_button_mcp23017_config_t;

/**
 * @brief Convenience default for the MCP23017 provider configuration.
 */
static inline espio_button_mcp23017_config_t espio_button_mcp23017_config_default(void) {
    espio_button_mcp23017_config_t config = {
        .i2c_port = 0,
        .sda_pin = ESPIO_BUTTON_PIN_UNUSED,
        .scl_pin = ESPIO_BUTTON_PIN_UNUSED,
        .device_address = 0x20,
        .scl_speed_hz = 100000U,
        .enable_bus_internal_pullups = false,
        .interrupt_pin = ESPIO_BUTTON_PIN_UNUSED,
        .interrupt_active_low = true,
        .transaction_timeout_ms = 100U,
        .channels = NULL,
        .channel_count = 0U
    };
    return config;
}
#define ESPIO_BUTTON_MCP23017_CONFIG_DEFAULT() espio_button_mcp23017_config_default()

/**
 * @brief Get the MCP23017 provider driver descriptor.
 * @return Immutable MCP23017 driver descriptor.
 */
const espio_button_driver_t* espio_button_driver_mcp23017(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_BUTTON_MCP23017_H */
