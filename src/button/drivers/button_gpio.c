/**
 * @file button_gpio.c
 * @brief Direct GPIO provider for the generic button abstraction API.
 */

#include "espio/drivers/button_gpio.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "espio/log.h"

#include <string.h>

/**
 * @brief Runtime state for one GPIO-backed physical channel.
 */
typedef struct button_gpio_channel_runtime {
    espio_button_gpio_channel_config_t config; ///< Deep-copied public configuration.
    bool last_active;                          ///< Last physical active state reported to the manager.
    struct button_gpio_runtime* owner;         ///< Back-pointer to the owning provider runtime.
} button_gpio_channel_runtime_t;

/**
 * @brief Full runtime object for the GPIO provider.
 */
typedef struct button_gpio_runtime {
    espio_button_driver_host_t host;           ///< Host callbacks used to wake the manager.
    uint32_t provider_index;                   ///< Internal provider index used in task notifications.
    button_gpio_channel_runtime_t* channels;   ///< Channel runtime array.
    size_t channel_count;                      ///< Number of GPIO-backed channels.
    bool use_interrupts;                       ///< True when at least one channel uses GPIO interrupts.
} button_gpio_runtime_t;

static const char* TAG = "button_gpio";

/**
 * @brief Translate the public pull-mode enum into the ESP-IDF GPIO pull mode.
 */
static gpio_pull_mode_t button_gpio_pull_mode_to_idf(espio_button_gpio_pull_mode_t pull_mode) {
    switch (pull_mode) {
        case ESPIO_BUTTON_GPIO_PULL_UP:
            return GPIO_PULLUP_ONLY;
        case ESPIO_BUTTON_GPIO_PULL_DOWN:
            return GPIO_PULLDOWN_ONLY;
        case ESPIO_BUTTON_GPIO_PULL_NONE:
        default:
            return GPIO_FLOATING;
    }
}

/**
 * @brief Read one GPIO channel and convert it into the provider-local active state.
 */
static bool button_gpio_read_active(const button_gpio_channel_runtime_t* channel) {
    int level = 0;

    if (!channel) {
        return false;
    }

    level = gpio_get_level((gpio_num_t)channel->config.pin);
    return channel->config.active_high ? (level != 0) : (level == 0);
}

/**
 * @brief ISR trampoline that only wakes the generic manager.
 */
static void IRAM_ATTR button_gpio_isr_handler(void* arg) {
    button_gpio_channel_runtime_t* channel = (button_gpio_channel_runtime_t*)arg;
    bool higher_priority_task_woken = false;

    if (!channel || !channel->owner) {
        return;
    }

    channel->owner->host.notify_from_isr(channel->owner->host.context,
                                         channel->owner->provider_index,
                                         &higher_priority_task_woken);

    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Query capabilities of the GPIO provider for one concrete configuration.
 */
static int32_t button_gpio_query_caps(const void* config, espio_button_driver_caps_t* caps) {
    const espio_button_gpio_config_t* cfg = (const espio_button_gpio_config_t*)config;
    bool all_channels_interrupt = true;
    bool use_interrupts = false;

    if (!cfg || !caps || !cfg->channels || cfg->channel_count == 0U) {
        return ESPIO_INVALID_ARG;
    }

    for (size_t channel_index = 0; channel_index < cfg->channel_count; channel_index++) {
        const espio_button_gpio_channel_config_t* channel = &cfg->channels[channel_index];
        if (channel->pin < 0) {
            return ESPIO_INVALID_ARG;
        }

        for (size_t other_index = channel_index + 1U; other_index < cfg->channel_count; other_index++) {
            if (channel->channel_id == cfg->channels[other_index].channel_id) {
                return ESPIO_INVALID_ARG;
            }
        }

        if (channel->enable_interrupt) {
            use_interrupts = true;
        } else {
            all_channels_interrupt = false;
        }
    }

    memset(caps, 0, sizeof(*caps));
    caps->source_family = ESPIO_BUTTON_SOURCE_GPIO;
    caps->notification = (use_interrupts && all_channels_interrupt)
                             ? ESPIO_BUTTON_NOTIFY_INTERRUPT
                             : ESPIO_BUTTON_NOTIFY_POLL;
    caps->channel_count = cfg->channel_count;
    caps->max_events_per_collect = cfg->channel_count;
    caps->supports_hardware_debounce = false;
    caps->supports_simultaneous_inputs = true;
    caps->supports_relative_events = false;
    caps->supports_wakeup_hint = use_interrupts;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Initialize runtime state for the GPIO provider.
 */
static void* button_gpio_init(const void* config, const espio_button_driver_host_t* host, uint32_t provider_index) {
    const espio_button_gpio_config_t* cfg = (const espio_button_gpio_config_t*)config;
    button_gpio_runtime_t* runtime = NULL;

    if (!cfg || !host || !cfg->channels || cfg->channel_count == 0U) {
        return NULL;
    }

    runtime = heap_caps_calloc(1, sizeof(button_gpio_runtime_t), MALLOC_CAP_DEFAULT);
    if (!runtime) {
        return NULL;
    }

    runtime->channels = heap_caps_calloc(cfg->channel_count,
                                         sizeof(button_gpio_channel_runtime_t),
                                         MALLOC_CAP_DEFAULT);
    if (!runtime->channels) {
        heap_caps_free(runtime);
        return NULL;
    }

    runtime->host = *host;
    runtime->provider_index = provider_index;
    runtime->channel_count = cfg->channel_count;

    for (size_t channel_index = 0; channel_index < cfg->channel_count; channel_index++) {
        const espio_button_gpio_channel_config_t* src = &cfg->channels[channel_index];
        button_gpio_channel_runtime_t* dst = &runtime->channels[channel_index];
        gpio_config_t idf_config = {0};

        dst->config = *src;
        dst->owner = runtime;

        idf_config.pin_bit_mask = (1ULL << (uint32_t)src->pin);
        idf_config.mode = GPIO_MODE_INPUT;
        idf_config.pull_up_en = src->pull_mode == ESPIO_BUTTON_GPIO_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        idf_config.pull_down_en = src->pull_mode == ESPIO_BUTTON_GPIO_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
        idf_config.intr_type = src->enable_interrupt ? GPIO_INTR_ANYEDGE : GPIO_INTR_DISABLE;

        if (gpio_config(&idf_config) != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to configure gpio %d", src->pin);
            heap_caps_free(runtime->channels);
            heap_caps_free(runtime);
            return NULL;
        }

        if (src->pull_mode == ESPIO_BUTTON_GPIO_PULL_NONE) {
            gpio_set_pull_mode((gpio_num_t)src->pin, button_gpio_pull_mode_to_idf(src->pull_mode));
        }

        dst->last_active = button_gpio_read_active(dst);
        runtime->use_interrupts = runtime->use_interrupts || src->enable_interrupt;
    }

    return runtime;
}

/**
 * @brief Deinitialize the GPIO provider runtime.
 */
static void button_gpio_deinit(void* ctx) {
    button_gpio_runtime_t* runtime = (button_gpio_runtime_t*)ctx;

    if (!runtime) {
        return;
    }

    heap_caps_free(runtime->channels);
    heap_caps_free(runtime);
}

/**
 * @brief Start GPIO interrupts after the generic manager worker is ready.
 */
static int32_t button_gpio_start(void* ctx) {
    button_gpio_runtime_t* runtime = (button_gpio_runtime_t*)ctx;

    if (!runtime) {
        return ESPIO_INVALID_ARG;
    }

    if (!runtime->use_interrupts) {
        return ESPIO_BUTTON_OK;
    }

    esp_err_t result = gpio_install_isr_service(0);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESPIO_LOGE(TAG, "failed to install gpio isr service");
        return ESPIO_IO;
    }

    for (size_t channel_index = 0; channel_index < runtime->channel_count; channel_index++) {
        button_gpio_channel_runtime_t* channel = &runtime->channels[channel_index];
        if (!channel->config.enable_interrupt) {
            continue;
        }

        if (gpio_isr_handler_add((gpio_num_t)channel->config.pin, button_gpio_isr_handler, channel) != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to add gpio isr for pin %d", channel->config.pin);
            return ESPIO_IO;
        }
    }

    return ESPIO_BUTTON_OK;
}

/**
 * @brief Stop GPIO interrupts owned by the provider.
 */
static void button_gpio_stop(void* ctx) {
    button_gpio_runtime_t* runtime = (button_gpio_runtime_t*)ctx;

    if (!runtime) {
        return;
    }

    for (size_t channel_index = 0; channel_index < runtime->channel_count; channel_index++) {
        button_gpio_channel_runtime_t* channel = &runtime->channels[channel_index];
        if (!channel->config.enable_interrupt) {
            continue;
        }
        gpio_isr_handler_remove((gpio_num_t)channel->config.pin);
    }
}

/**
 * @brief Collect physical GPIO changes since the last collection pass.
 */
static int32_t button_gpio_collect(void* ctx,
                                   espio_button_collect_reason_t reason,
                                   espio_button_physical_change_t* changes,
                                   size_t capacity,
                                   size_t* out_count) {
    button_gpio_runtime_t* runtime = (button_gpio_runtime_t*)ctx;
    size_t produced = 0U;

    if (!runtime || !changes || capacity < runtime->channel_count || !out_count) {
        return ESPIO_INVALID_ARG;
    }

    for (size_t channel_index = 0; channel_index < runtime->channel_count; channel_index++) {
        button_gpio_channel_runtime_t* channel = &runtime->channels[channel_index];
        bool active = button_gpio_read_active(channel);

        if (reason != ESPIO_BUTTON_COLLECT_SYNC && active == channel->last_active) {
            continue;
        }

        changes[produced].channel_id = channel->config.channel_id;
        changes[produced].active = active;
        changes[produced].timestamp_us = runtime->host.get_time_us(runtime->host.context);
        channel->last_active = active;
        produced++;
    }

    *out_count = produced;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Immutable public driver descriptor for the GPIO provider.
 */
const espio_button_driver_t* espio_button_driver_gpio(void) {
    static const espio_button_driver_t driver = {
        .name = "button_gpio",
        .query_caps = button_gpio_query_caps,
        .init = button_gpio_init,
        .deinit = button_gpio_deinit,
        .start = button_gpio_start,
        .stop = button_gpio_stop,
        .collect = button_gpio_collect,
    };

    return &driver;
}
