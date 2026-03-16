/**
 * @file button_mcp23017.c
 * @brief MCP23017 I2C GPIO expander provider for the generic button API.
 */

#include "espio/drivers/button_mcp23017.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "espio/log.h"

#include <string.h>

/**
 * @brief MCP23017 register map used by the provider with IOCON.BANK kept at zero.
 */
#define BUTTON_MCP23017_REG_IODIRA     0x00U
#define BUTTON_MCP23017_REG_IODIRB     0x01U
#define BUTTON_MCP23017_REG_GPINTENA   0x04U
#define BUTTON_MCP23017_REG_GPINTENB   0x05U
#define BUTTON_MCP23017_REG_DEFVALA    0x06U
#define BUTTON_MCP23017_REG_DEFVALB    0x07U
#define BUTTON_MCP23017_REG_INTCONA    0x08U
#define BUTTON_MCP23017_REG_INTCONB    0x09U
#define BUTTON_MCP23017_REG_IOCONA     0x0AU
#define BUTTON_MCP23017_REG_IOCONB     0x0BU
#define BUTTON_MCP23017_REG_GPPUA      0x0CU
#define BUTTON_MCP23017_REG_GPPUB      0x0DU
#define BUTTON_MCP23017_REG_INTFA      0x0EU
#define BUTTON_MCP23017_REG_INTFB      0x0FU
#define BUTTON_MCP23017_REG_INTCAPA    0x10U
#define BUTTON_MCP23017_REG_INTCAPB    0x11U
#define BUTTON_MCP23017_REG_GPIOA      0x12U
#define BUTTON_MCP23017_REG_GPIOB      0x13U

/**
 * @brief IOCON bit definitions used by the provider.
 */
#define BUTTON_MCP23017_IOCON_MIRROR   0x40U
#define BUTTON_MCP23017_IOCON_INTPOL   0x02U

/**
 * @brief Runtime state for one MCP23017-backed physical channel.
 */
typedef struct {
    espio_button_mcp23017_channel_config_t config; ///< Deep-copied public configuration.
    bool last_active;                          ///< Last physical active state reported to the manager.
} button_mcp23017_channel_runtime_t;

/**
 * @brief Full runtime object for the MCP23017 provider.
 */
typedef struct {
    espio_button_driver_host_t host;                  ///< Host callbacks used to wake the manager.
    uint32_t provider_index;                          ///< Internal provider index used by task notifications.
    i2c_master_bus_handle_t bus_handle;               ///< Provider-owned ESP-IDF I2C bus handle.
    i2c_master_dev_handle_t device_handle;            ///< Provider-owned MCP23017 device handle.
    button_mcp23017_channel_runtime_t* channels;      ///< Deep-copied channel configuration array.
    size_t channel_count;                             ///< Number of configured channels.
    uint32_t timeout_ms;                              ///< I2C transaction timeout in milliseconds.
    int interrupt_pin;                                ///< Optional ESP32 GPIO connected to the expander interrupt output.
    bool interrupt_active_low;                        ///< Interrupt polarity used by the ESP32 side.
    bool use_interrupt;                               ///< True when the provider uses the MCP23017 interrupt output.
    uint8_t last_port_state[2];                       ///< Last raw GPIOA and GPIOB register values.
} button_mcp23017_runtime_t;

static const char* TAG = "button_mcp23017";

/**
 * @brief Translate an ESP-IDF error into a public button error.
 */
static int32_t button_mcp23017_translate_esp_error(esp_err_t error) {
    switch (error) {
        case ESP_OK:
            return ESPIO_BUTTON_OK;
        case ESP_ERR_INVALID_ARG:
            return ESPIO_INVALID_ARG;
        case ESP_ERR_NO_MEM:
            return ESPIO_NO_MEM;
        case ESP_ERR_TIMEOUT:
            return ESPIO_TIMEOUT;
        case ESP_ERR_NOT_SUPPORTED:
            return ESPIO_UNSUPPORTED;
        default:
            return ESPIO_IO;
    }
}

/**
 * @brief Read the logical active state of one runtime channel from the raw port-state cache.
 */
static bool button_mcp23017_channel_active(const button_mcp23017_channel_runtime_t* channel,
                                           const uint8_t port_state[2]) {
    uint8_t bit_value = 0U;

    if (!channel || !port_state) {
        return false;
    }

    bit_value = (uint8_t)((port_state[channel->config.port] >> channel->config.pin) & 0x01U);
    return channel->config.active_high ? (bit_value != 0U) : (bit_value == 0U);
}

/**
 * @brief Write one MCP23017 register.
 */
static int32_t button_mcp23017_write_register(button_mcp23017_runtime_t* runtime,
                                              uint8_t reg,
                                              uint8_t value) {
    uint8_t buffer[2];
    esp_err_t result;

    if (!runtime || !runtime->device_handle) {
        return ESPIO_INVALID_ARG;
    }

    buffer[0] = reg;
    buffer[1] = value;
    result = i2c_master_transmit(runtime->device_handle, buffer, sizeof(buffer), (int)runtime->timeout_ms);
    return button_mcp23017_translate_esp_error(result);
}

/**
 * @brief Write one sequential register pair to the MCP23017.
 */
static int32_t button_mcp23017_write_register_pair(button_mcp23017_runtime_t* runtime,
                                                   uint8_t start_reg,
                                                   uint8_t first_value,
                                                   uint8_t second_value) {
    uint8_t buffer[3];
    esp_err_t result;

    if (!runtime || !runtime->device_handle) {
        return ESPIO_INVALID_ARG;
    }

    buffer[0] = start_reg;
    buffer[1] = first_value;
    buffer[2] = second_value;
    result = i2c_master_transmit(runtime->device_handle, buffer, sizeof(buffer), (int)runtime->timeout_ms);
    return button_mcp23017_translate_esp_error(result);
}

/**
 * @brief Read one sequential register pair from the MCP23017.
 */
static int32_t button_mcp23017_read_register_pair(button_mcp23017_runtime_t* runtime,
                                                  uint8_t start_reg,
                                                  uint8_t values[2]) {
    esp_err_t result;

    if (!runtime || !runtime->device_handle || !values) {
        return ESPIO_INVALID_ARG;
    }

    result = i2c_master_transmit_receive(runtime->device_handle,
                                         &start_reg,
                                         sizeof(start_reg),
                                         values,
                                         2U,
                                         (int)runtime->timeout_ms);
    return button_mcp23017_translate_esp_error(result);
}

/**
 * @brief Initialize MCP23017 register state for input-only button handling.
 */
static int32_t button_mcp23017_configure_device(button_mcp23017_runtime_t* runtime) {
    uint8_t iodir_a = 0xFFU;
    uint8_t iodir_b = 0xFFU;
    uint8_t gpinten_a = 0x00U;
    uint8_t gpinten_b = 0x00U;
    uint8_t defval_a = 0x00U;
    uint8_t defval_b = 0x00U;
    uint8_t intcon_a = 0x00U;
    uint8_t intcon_b = 0x00U;
    uint8_t gppu_a = 0x00U;
    uint8_t gppu_b = 0x00U;
    uint8_t iocon = 0x00U;
    int32_t result = ESPIO_BUTTON_OK;

    if (!runtime) {
        return ESPIO_INVALID_ARG;
    }

    for (size_t channel_index = 0; channel_index < runtime->channel_count; channel_index++) {
        const button_mcp23017_channel_runtime_t* channel = &runtime->channels[channel_index];
        uint8_t bit_mask = (uint8_t)(1U << channel->config.pin);

        if (channel->config.port == ESPIO_BUTTON_MCP23017_PORT_A) {
            if (channel->config.enable_pullup) {
                gppu_a |= bit_mask;
            }
            if (runtime->use_interrupt) {
                gpinten_a |= bit_mask;
            }
        } else {
            if (channel->config.enable_pullup) {
                gppu_b |= bit_mask;
            }
            if (runtime->use_interrupt) {
                gpinten_b |= bit_mask;
            }
        }
    }

    if (runtime->use_interrupt) {
        iocon |= BUTTON_MCP23017_IOCON_MIRROR;
        if (!runtime->interrupt_active_low) {
            iocon |= BUTTON_MCP23017_IOCON_INTPOL;
        }
    }

    result = button_mcp23017_write_register_pair(runtime, BUTTON_MCP23017_REG_IOCONA, iocon, iocon);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    result = button_mcp23017_write_register_pair(runtime, BUTTON_MCP23017_REG_IODIRA, iodir_a, iodir_b);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    result = button_mcp23017_write_register_pair(runtime, BUTTON_MCP23017_REG_GPPUA, gppu_a, gppu_b);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    result = button_mcp23017_write_register_pair(runtime, BUTTON_MCP23017_REG_DEFVALA, defval_a, defval_b);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    result = button_mcp23017_write_register_pair(runtime, BUTTON_MCP23017_REG_INTCONA, intcon_a, intcon_b);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    result = button_mcp23017_write_register_pair(runtime, BUTTON_MCP23017_REG_GPINTENA, gpinten_a, gpinten_b);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    return button_mcp23017_read_register_pair(runtime, BUTTON_MCP23017_REG_GPIOA, runtime->last_port_state);
}

/**
 * @brief Configure the ESP32 GPIO connected to the MCP23017 interrupt output.
 */
static int32_t button_mcp23017_configure_interrupt_pin(button_mcp23017_runtime_t* runtime) {
    gpio_config_t gpio_config_data = {0};
    esp_err_t result;

    if (!runtime || !runtime->use_interrupt) {
        return ESPIO_BUTTON_OK;
    }

    gpio_config_data.pin_bit_mask = (1ULL << (uint32_t)runtime->interrupt_pin);
    gpio_config_data.mode = GPIO_MODE_INPUT;
    gpio_config_data.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config_data.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config_data.intr_type = runtime->interrupt_active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE;

    result = gpio_config(&gpio_config_data);
    return button_mcp23017_translate_esp_error(result);
}

/**
 * @brief ISR trampoline that only wakes the generic manager task.
 */
static void IRAM_ATTR button_mcp23017_isr_handler(void* arg) {
    button_mcp23017_runtime_t* runtime = (button_mcp23017_runtime_t*)arg;
    bool higher_priority_task_woken = false;

    if (!runtime) {
        return;
    }

    runtime->host.notify_from_isr(runtime->host.context, runtime->provider_index, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Query capabilities of the MCP23017 provider for one concrete configuration.
 */
static int32_t button_mcp23017_query_caps(const void* config, espio_button_driver_caps_t* caps) {
    const espio_button_mcp23017_config_t* mcp_config = (const espio_button_mcp23017_config_t*)config;

    if (!mcp_config || !caps || !mcp_config->channels || mcp_config->channel_count == 0U) {
        return ESPIO_INVALID_ARG;
    }

    if (mcp_config->i2c_port < 0 || mcp_config->sda_pin < 0 || mcp_config->scl_pin < 0) {
        return ESPIO_INVALID_ARG;
    }

    if (mcp_config->interrupt_pin < ESPIO_BUTTON_PIN_UNUSED) {
        return ESPIO_INVALID_ARG;
    }

    if (mcp_config->device_address < 0x20U || mcp_config->device_address > 0x27U) {
        return ESPIO_INVALID_ARG;
    }

    for (size_t channel_index = 0; channel_index < mcp_config->channel_count; channel_index++) {
        const espio_button_mcp23017_channel_config_t* channel = &mcp_config->channels[channel_index];
        if (channel->port != ESPIO_BUTTON_MCP23017_PORT_A && channel->port != ESPIO_BUTTON_MCP23017_PORT_B) {
            return ESPIO_INVALID_ARG;
        }
        if (channel->pin > 7U) {
            return ESPIO_INVALID_ARG;
        }

        for (size_t other_index = channel_index + 1U; other_index < mcp_config->channel_count; other_index++) {
            const espio_button_mcp23017_channel_config_t* other = &mcp_config->channels[other_index];
            if (channel->channel_id == other->channel_id) {
                return ESPIO_INVALID_ARG;
            }
            if (channel->port == other->port && channel->pin == other->pin) {
                return ESPIO_INVALID_ARG;
            }
        }
    }

    memset(caps, 0, sizeof(*caps));
    caps->source_family = ESPIO_BUTTON_SOURCE_EXPANDER;
    caps->notification = mcp_config->interrupt_pin == ESPIO_BUTTON_PIN_UNUSED
                             ? ESPIO_BUTTON_NOTIFY_POLL
                             : ESPIO_BUTTON_NOTIFY_INTERRUPT;
    caps->channel_count = mcp_config->channel_count;
    caps->max_events_per_collect = mcp_config->channel_count;
    caps->supports_hardware_debounce = false;
    caps->supports_simultaneous_inputs = true;
    caps->supports_relative_events = false;
    caps->supports_wakeup_hint = mcp_config->interrupt_pin != ESPIO_BUTTON_PIN_UNUSED;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Initialize runtime state for the MCP23017 provider.
 */
static void* button_mcp23017_init(const void* config, const espio_button_driver_host_t* host, uint32_t provider_index) {
    const espio_button_mcp23017_config_t* mcp_config = (const espio_button_mcp23017_config_t*)config;
    button_mcp23017_runtime_t* runtime = NULL;
    i2c_master_bus_config_t bus_config = {0};
    i2c_device_config_t device_config = {0};
    esp_err_t result;

    if (!mcp_config || !host || !mcp_config->channels || mcp_config->channel_count == 0U) {
        return NULL;
    }

    runtime = heap_caps_calloc(1, sizeof(button_mcp23017_runtime_t), MALLOC_CAP_DEFAULT);
    if (!runtime) {
        return NULL;
    }

    runtime->channels = heap_caps_calloc(mcp_config->channel_count,
                                         sizeof(button_mcp23017_channel_runtime_t),
                                         MALLOC_CAP_DEFAULT);
    if (!runtime->channels) {
        heap_caps_free(runtime);
        return NULL;
    }

    runtime->host = *host;
    runtime->provider_index = provider_index;
    runtime->channel_count = mcp_config->channel_count;
    runtime->timeout_ms = mcp_config->transaction_timeout_ms;
    runtime->interrupt_pin = mcp_config->interrupt_pin;
    runtime->interrupt_active_low = mcp_config->interrupt_active_low;
    runtime->use_interrupt = mcp_config->interrupt_pin != ESPIO_BUTTON_PIN_UNUSED;
    if (runtime->timeout_ms == 0U) {
        runtime->timeout_ms = 100U;
    }

    for (size_t channel_index = 0; channel_index < mcp_config->channel_count; channel_index++) {
        runtime->channels[channel_index].config = mcp_config->channels[channel_index];
    }

    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.i2c_port = mcp_config->i2c_port;
    bus_config.sda_io_num = mcp_config->sda_pin;
    bus_config.scl_io_num = mcp_config->scl_pin;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = mcp_config->enable_bus_internal_pullups ? 1U : 0U;

    result = i2c_new_master_bus(&bus_config, &runtime->bus_handle);
    if (result != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to create i2c bus for mcp23017");
        heap_caps_free(runtime->channels);
        heap_caps_free(runtime);
        return NULL;
    }

    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = mcp_config->device_address;
    device_config.scl_speed_hz = mcp_config->scl_speed_hz;
    if (device_config.scl_speed_hz == 0U) {
        device_config.scl_speed_hz = 100000U;
    }

    result = i2c_master_bus_add_device(runtime->bus_handle, &device_config, &runtime->device_handle);
    if (result != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to add mcp23017 device to i2c bus");
        i2c_del_master_bus(runtime->bus_handle);
        heap_caps_free(runtime->channels);
        heap_caps_free(runtime);
        return NULL;
    }

    if (button_mcp23017_configure_device(runtime) != ESPIO_BUTTON_OK) {
        i2c_master_bus_rm_device(runtime->device_handle);
        i2c_del_master_bus(runtime->bus_handle);
        heap_caps_free(runtime->channels);
        heap_caps_free(runtime);
        return NULL;
    }

    for (size_t channel_index = 0; channel_index < runtime->channel_count; channel_index++) {
        runtime->channels[channel_index].last_active =
            button_mcp23017_channel_active(&runtime->channels[channel_index], runtime->last_port_state);
    }

    if (button_mcp23017_configure_interrupt_pin(runtime) != ESPIO_BUTTON_OK) {
        i2c_master_bus_rm_device(runtime->device_handle);
        i2c_del_master_bus(runtime->bus_handle);
        heap_caps_free(runtime->channels);
        heap_caps_free(runtime);
        return NULL;
    }

    return runtime;
}

/**
 * @brief Deinitialize the MCP23017 provider runtime.
 */
static void button_mcp23017_deinit(void* ctx) {
    button_mcp23017_runtime_t* runtime = (button_mcp23017_runtime_t*)ctx;

    if (!runtime) {
        return;
    }

    if (runtime->device_handle) {
        i2c_master_bus_rm_device(runtime->device_handle);
    }
    if (runtime->bus_handle) {
        i2c_del_master_bus(runtime->bus_handle);
    }

    heap_caps_free(runtime->channels);
    heap_caps_free(runtime);
}

/**
 * @brief Start interrupt handling for the MCP23017 provider.
 */
static int32_t button_mcp23017_start(void* ctx) {
    button_mcp23017_runtime_t* runtime = (button_mcp23017_runtime_t*)ctx;
    esp_err_t result;

    if (!runtime) {
        return ESPIO_INVALID_ARG;
    }

    if (!runtime->use_interrupt) {
        return ESPIO_BUTTON_OK;
    }

    result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESPIO_LOGE(TAG, "failed to install gpio isr service for mcp23017");
        return ESPIO_IO;
    }

    result = gpio_isr_handler_add((gpio_num_t)runtime->interrupt_pin, button_mcp23017_isr_handler, runtime);
    return button_mcp23017_translate_esp_error(result);
}

/**
 * @brief Stop interrupt handling for the MCP23017 provider.
 */
static void button_mcp23017_stop(void* ctx) {
    button_mcp23017_runtime_t* runtime = (button_mcp23017_runtime_t*)ctx;

    if (!runtime || !runtime->use_interrupt) {
        return;
    }

    gpio_isr_handler_remove((gpio_num_t)runtime->interrupt_pin);
}

/**
 * @brief Read the raw MCP23017 port state, preferring INTCAP for interrupt-driven collection.
 */
static int32_t button_mcp23017_read_ports(button_mcp23017_runtime_t* runtime,
                                          espio_button_collect_reason_t reason,
                                          uint8_t port_state[2]) {
    int32_t result = ESPIO_BUTTON_OK;

    if (!runtime || !port_state) {
        return ESPIO_INVALID_ARG;
    }

    if (reason == ESPIO_BUTTON_COLLECT_INTERRUPT && runtime->use_interrupt) {
        uint8_t interrupt_flags[2] = {0U, 0U};

        result = button_mcp23017_read_register_pair(runtime, BUTTON_MCP23017_REG_INTFA, interrupt_flags);
        if (result != ESPIO_BUTTON_OK) {
            return result;
        }

        if (interrupt_flags[0] != 0U || interrupt_flags[1] != 0U) {
            return button_mcp23017_read_register_pair(runtime, BUTTON_MCP23017_REG_INTCAPA, port_state);
        }
    }

    return button_mcp23017_read_register_pair(runtime, BUTTON_MCP23017_REG_GPIOA, port_state);
}

/**
 * @brief Collect physical MCP23017 changes since the last collection pass.
 */
static int32_t button_mcp23017_collect(void* ctx,
                                       espio_button_collect_reason_t reason,
                                       espio_button_physical_change_t* changes,
                                       size_t capacity,
                                       size_t* out_count) {
    button_mcp23017_runtime_t* runtime = (button_mcp23017_runtime_t*)ctx;
    uint8_t port_state[2] = {0U, 0U};
    size_t produced = 0U;
    int32_t result = ESPIO_BUTTON_OK;

    if (!runtime || !changes || !out_count || capacity < runtime->channel_count) {
        return ESPIO_INVALID_ARG;
    }

    result = button_mcp23017_read_ports(runtime, reason, port_state);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    for (size_t channel_index = 0; channel_index < runtime->channel_count; channel_index++) {
        button_mcp23017_channel_runtime_t* channel = &runtime->channels[channel_index];
        bool active = button_mcp23017_channel_active(channel, port_state);

        if (reason != ESPIO_BUTTON_COLLECT_SYNC && active == channel->last_active) {
            continue;
        }

        changes[produced].channel_id = channel->config.channel_id;
        changes[produced].active = active;
        changes[produced].timestamp_us = runtime->host.get_time_us(runtime->host.context);
        channel->last_active = active;
        produced++;
    }

    runtime->last_port_state[0] = port_state[0];
    runtime->last_port_state[1] = port_state[1];
    *out_count = produced;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Immutable public driver descriptor for the MCP23017 provider.
 */
const espio_button_driver_t* espio_button_driver_mcp23017(void) {
    static const espio_button_driver_t driver = {
        .name = "button_mcp23017",
        .query_caps = button_mcp23017_query_caps,
        .init = button_mcp23017_init,
        .deinit = button_mcp23017_deinit,
        .start = button_mcp23017_start,
        .stop = button_mcp23017_stop,
        .collect = button_mcp23017_collect,
    };

    return &driver;
}
