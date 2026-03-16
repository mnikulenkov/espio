/**
 * @file button.c
 * @brief Generic button manager facade and state machine.
 */

#include "espio/button.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "espio/log.h"

#include <string.h>

/**
 * @brief Internal provider limit imposed by task-notification bit width.
 */
#define BUTTON_MAX_PROVIDER_COUNT 31U

/**
 * @brief Stop bit stored in the worker-task notification word.
 */
#define BUTTON_NOTIFY_STOP_BIT (1UL << 31)

/**
 * @brief Internal marker that no deadline is currently scheduled.
 */
#define BUTTON_TIME_NONE UINT64_MAX

static const char* TAG = "button";

/**
 * @brief Runtime copy of one provider registration.
 */
typedef struct {
    uint32_t provider_id;                      ///< Stable public provider identifier.
    const espio_button_driver_t* driver;      ///< Concrete provider implementation.
    void* driver_ctx;                         ///< Driver-owned runtime context.
    espio_button_driver_caps_t caps;          ///< Capability snapshot returned during creation.
    uint32_t poll_period_ms;                  ///< Effective poll period used by the worker.
    uint64_t next_poll_due_us;                ///< Next scheduled poll time.
} button_provider_runtime_t;

/**
 * @brief Internal source cache entry used for binding resolution.
 */
typedef struct {
    uint32_t provider_id;                      ///< Stable provider identifier.
    espio_button_channel_id_t channel_id;      ///< Provider-local channel identifier.
    bool active;                               ///< Last known physical active state.
} button_source_cache_entry_t;

/**
 * @brief Runtime state for one logical binding.
 */
typedef struct {
    espio_button_binding_t config;            ///< Deep-copied binding configuration.
    size_t* source_indices;                    ///< Indices into the shared source cache.
    espio_button_state_snapshot_t snapshot;   ///< Publicly visible state snapshot.
    bool raw_active;                           ///< Most recent aggregate raw state.
    bool stable_active;                        ///< Last committed stable aggregate state.
    bool pending_active;                       ///< Candidate state waiting for debounce.
    bool debounce_pending;                     ///< True while a debounce decision is pending.
    uint64_t debounce_due_us;                  ///< Debounce commit deadline.
    uint64_t pressed_since_us;                 ///< Stable press start time.
    uint64_t next_repeat_us;                   ///< Next repeat deadline.
} button_binding_runtime_t;

/**
 * @brief Full manager runtime object hidden behind the public facade.
 */
struct espio_button {
    button_provider_runtime_t* providers;      ///< Provider runtime array.
    size_t provider_count;                     ///< Number of providers owned by the manager.
    button_binding_runtime_t* bindings;        ///< Logical binding runtime array.
    size_t binding_count;                      ///< Number of logical bindings.
    button_source_cache_entry_t* sources;      ///< Unique source cache used by bindings.
    size_t source_count;                       ///< Number of unique source cache entries.
    espio_button_service_config_t service;    ///< Effective service configuration.
    espio_button_physical_change_t* scratch_changes; ///< Shared provider collection buffer.
    size_t scratch_capacity;                   ///< Capacity of the shared collection buffer.
    QueueHandle_t event_queue;                 ///< Semantic event queue consumed by the application.
    SemaphoreHandle_t lock;                    ///< Mutex protecting state snapshots and diagnostics.
    TaskHandle_t task_handle;                  ///< Worker task handle.
    int32_t last_error;                        ///< Last public error recorded by the manager.
    uint32_t dropped_event_count;              ///< Number of dropped semantic events.
    uint32_t next_sequence;                    ///< Next semantic event sequence number.
    bool stop_requested;                       ///< True after destruction starts.
    bool task_stopped;                         ///< True after the worker task exits.
} ;

/**
 * @brief Look up the current monotonic time used by the manager.
 */
static uint64_t button_now_us(void* context) {
    (void)context;
    return (uint64_t)esp_timer_get_time();
}

/**
 * @brief Wake the worker task from normal task context.
 */
static void button_host_notify(void* context, uint32_t provider_index) {
    espio_button_t* button = (espio_button_t*)context;
    if (!button || !button->task_handle || provider_index >= BUTTON_MAX_PROVIDER_COUNT) {
        return;
    }

    xTaskNotify(button->task_handle, (1UL << provider_index), eSetBits);
}

/**
 * @brief Wake the worker task from ISR context.
 */
static void button_host_notify_from_isr(void* context, uint32_t provider_index, bool* higher_priority_task_woken) {
    espio_button_t* button = (espio_button_t*)context;
    BaseType_t task_woken = pdFALSE;

    if (!button || !button->task_handle || provider_index >= BUTTON_MAX_PROVIDER_COUNT) {
        if (higher_priority_task_woken) {
            *higher_priority_task_woken = false;
        }
        return;
    }

    xTaskNotifyFromISR(button->task_handle, (1UL << provider_index), eSetBits, &task_woken);

    if (higher_priority_task_woken) {
        *higher_priority_task_woken = (task_woken == pdTRUE);
    }
}

/**
 * @brief Record the last public error while keeping the existing API observable.
 */
static int32_t button_record_error(espio_button_t* button, int32_t error) {
    if (button && button->lock) {
        if (xSemaphoreTake(button->lock, portMAX_DELAY) == pdTRUE) {
            button->last_error = error;
            xSemaphoreGive(button->lock);
        }
    }
    return error;
}

/**
 * @brief Apply the service-level default behavior to one binding explicitly.
 */
static void button_apply_behavior_defaults(espio_button_behavior_t* behavior, const espio_button_service_config_t* service) {
    if (!behavior || !service) {
        return;
    }

    *behavior = service->default_behavior;
}

/**
 * @brief Return the provider runtime that owns one stable provider identifier.
 */
static button_provider_runtime_t* button_find_provider(espio_button_t* button, uint32_t provider_id) {
    if (!button) {
        return NULL;
    }

    for (size_t index = 0; index < button->provider_count; index++) {
        if (button->providers[index].provider_id == provider_id) {
            return &button->providers[index];
        }
    }

    return NULL;
}

/**
 * @brief Return the unique source-cache index for one source reference, creating it when needed.
 */
static int32_t button_get_or_add_source(espio_button_t* button,
                                        const espio_button_source_ref_t* source,
                                        size_t next_index,
                                        size_t* out_index) {
    if (!button || !source || !out_index) {
        return ESPIO_INVALID_ARG;
    }

    for (size_t index = 0; index < next_index; index++) {
        if (button->sources[index].provider_id == source->provider_id &&
            button->sources[index].channel_id == source->channel_id) {
            *out_index = index;
            return ESPIO_BUTTON_OK;
        }
    }

    button->sources[next_index].provider_id = source->provider_id;
    button->sources[next_index].channel_id = source->channel_id;
    button->sources[next_index].active = false;
    *out_index = next_index;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Evaluate the aggregate raw state of one logical binding.
 */
static bool button_evaluate_binding_raw_state(const espio_button_t* button, const button_binding_runtime_t* binding) {
    if (!button || !binding || binding->config.source_count == 0U) {
        return false;
    }

    if (binding->config.mode == ESPIO_BUTTON_BIND_ALL) {
        for (size_t source_index = 0; source_index < binding->config.source_count; source_index++) {
            if (!button->sources[binding->source_indices[source_index]].active) {
                return false;
            }
        }
        return true;
    }

    for (size_t source_index = 0; source_index < binding->config.source_count; source_index++) {
        if (button->sources[binding->source_indices[source_index]].active) {
            return true;
        }
    }

    return false;
}

/**
 * @brief Push one semantic event into the public queue or record overflow when the queue is full.
 */
static void button_emit_event(espio_button_t* button,
                              button_binding_runtime_t* binding,
                              espio_button_event_type_t type,
                              espio_button_state_t state,
                              uint64_t timestamp_us) {
    espio_button_event_t event;

    if (!button || !binding) {
        return;
    }

    event.button_id = binding->config.button_id;
    event.type = type;
    event.state = state;
    event.timestamp_us = timestamp_us;
    event.sequence = ++button->next_sequence;

    if (xQueueSend(button->event_queue, &event, 0U) != pdTRUE) {
        button->dropped_event_count++;
        button->last_error = ESPIO_BUTTON_ERR_QUEUE_FULL;
        return;
    }

    binding->snapshot.last_event_time_us = timestamp_us;
}

/**
 * @brief Apply a stable state transition after debounce and emit semantic events when configured.
 */
static void button_commit_stable_state(espio_button_t* button,
                                       button_binding_runtime_t* binding,
                                       bool active,
                                       uint64_t timestamp_us) {
    if (!button || !binding) {
        return;
    }

    binding->snapshot.state = active ? ESPIO_BUTTON_PRESSED : ESPIO_BUTTON_RELEASED;
    binding->snapshot.last_change_time_us = timestamp_us;
    binding->snapshot.long_press_reported = false;
    binding->debounce_pending = false;
    binding->debounce_due_us = BUTTON_TIME_NONE;
    binding->stable_active = active;

    if (active) {
        binding->pressed_since_us = timestamp_us;
        if (binding->config.behavior.repeat_delay_ms > 0U &&
            binding->config.behavior.repeat_interval_ms > 0U &&
            binding->config.behavior.emit_repeat) {
            binding->next_repeat_us = timestamp_us +
                                      ((uint64_t)binding->config.behavior.repeat_delay_ms * 1000ULL);
        } else {
            binding->next_repeat_us = BUTTON_TIME_NONE;
        }

        if (binding->config.behavior.emit_press) {
            button_emit_event(button, binding, ESPIO_BUTTON_EVENT_PRESS, ESPIO_BUTTON_PRESSED, timestamp_us);
        }
        return;
    }

    binding->pressed_since_us = BUTTON_TIME_NONE;
    binding->next_repeat_us = BUTTON_TIME_NONE;
    if (binding->config.behavior.emit_release) {
        button_emit_event(button, binding, ESPIO_BUTTON_EVENT_RELEASE, ESPIO_BUTTON_RELEASED, timestamp_us);
    }
}

/**
 * @brief Update binding debounce state after one raw aggregate change.
 */
static void button_schedule_binding_transition(button_binding_runtime_t* binding,
                                               bool active,
                                               uint64_t timestamp_us) {
    if (!binding) {
        return;
    }

    binding->raw_active = active;
    binding->pending_active = active;

    if (binding->config.behavior.debounce_ms == 0U) {
        binding->debounce_pending = true;
        binding->debounce_due_us = timestamp_us;
        return;
    }

    binding->debounce_pending = true;
    binding->debounce_due_us = timestamp_us + ((uint64_t)binding->config.behavior.debounce_ms * 1000ULL);
}

/**
 * @brief Seed one binding from synchronized provider state without emitting semantic events.
 */
static void button_seed_binding_state(button_binding_runtime_t* binding, bool active, uint64_t timestamp_us) {
    if (!binding) {
        return;
    }

    binding->raw_active = active;
    binding->pending_active = active;
    binding->debounce_pending = false;
    binding->debounce_due_us = BUTTON_TIME_NONE;
    binding->snapshot.state = active ? ESPIO_BUTTON_PRESSED : ESPIO_BUTTON_RELEASED;
    binding->snapshot.last_change_time_us = timestamp_us;
    binding->snapshot.last_event_time_us = 0U;
    binding->snapshot.long_press_reported = false;
    binding->stable_active = active;

    if (active) {
        binding->pressed_since_us = timestamp_us;
        if (binding->config.behavior.repeat_delay_ms > 0U &&
            binding->config.behavior.repeat_interval_ms > 0U &&
            binding->config.behavior.emit_repeat) {
            binding->next_repeat_us = timestamp_us +
                                      ((uint64_t)binding->config.behavior.repeat_delay_ms * 1000ULL);
        } else {
            binding->next_repeat_us = BUTTON_TIME_NONE;
        }
    } else {
        binding->pressed_since_us = BUTTON_TIME_NONE;
        binding->next_repeat_us = BUTTON_TIME_NONE;
    }
}

/**
 * @brief Process one physical change batch produced by a provider collection pass.
 */
static int32_t button_process_changes(espio_button_t* button,
                                      button_provider_runtime_t* provider,
                                      const espio_button_physical_change_t* changes,
                                      size_t change_count,
                                      bool initial_sync) {
    if (!button || !provider) {
        return ESPIO_INVALID_ARG;
    }

    for (size_t change_index = 0; change_index < change_count; change_index++) {
        const espio_button_physical_change_t* change = &changes[change_index];
        uint64_t timestamp_us = change->timestamp_us != 0U ? change->timestamp_us : button_now_us(NULL);

        for (size_t source_index = 0; source_index < button->source_count; source_index++) {
            if (button->sources[source_index].provider_id == provider->provider_id &&
                button->sources[source_index].channel_id == change->channel_id) {
                button->sources[source_index].active = change->active;
            }
        }

        for (size_t binding_index = 0; binding_index < button->binding_count; binding_index++) {
            button_binding_runtime_t* binding = &button->bindings[binding_index];
            bool references_provider_channel = false;

            for (size_t source_ref_index = 0; source_ref_index < binding->config.source_count; source_ref_index++) {
                size_t source_cache_index = binding->source_indices[source_ref_index];
                if (button->sources[source_cache_index].provider_id == provider->provider_id &&
                    button->sources[source_cache_index].channel_id == change->channel_id) {
                    references_provider_channel = true;
                    break;
                }
            }

            if (!references_provider_channel) {
                continue;
            }

            bool aggregate_active = button_evaluate_binding_raw_state(button, binding);
            if (initial_sync) {
                button_seed_binding_state(binding, aggregate_active, timestamp_us);
                continue;
            }

            if (aggregate_active != binding->raw_active) {
                button_schedule_binding_transition(binding, aggregate_active, timestamp_us);
            }
        }
    }

    return ESPIO_BUTTON_OK;
}

/**
 * @brief Collect changes from one provider for the requested reason.
 */
static int32_t button_collect_provider(espio_button_t* button,
                                       button_provider_runtime_t* provider,
                                       espio_button_collect_reason_t reason,
                                       bool initial_sync) {
    size_t change_count = 0U;
    int32_t result;

    if (!button || !provider || !provider->driver || !provider->driver->collect) {
        return ESPIO_INVALID_ARG;
    }

    result = provider->driver->collect(provider->driver_ctx,
                                       reason,
                                       button->scratch_changes,
                                       button->scratch_capacity,
                                       &change_count);
    if (result != ESPIO_BUTTON_OK) {
        ESPIO_LOGE(TAG, "provider %s collect failed: %s", provider->driver->name, espio_button_err_to_name(result));
        return button_record_error(button, result);
    }

    if (change_count > button->scratch_capacity) {
        return button_record_error(button, ESPIO_INTERNAL);
    }

    if (xSemaphoreTake(button->lock, portMAX_DELAY) != pdTRUE) {
        return button_record_error(button, ESPIO_BUSY);
    }

    result = button_process_changes(button, provider, button->scratch_changes, change_count, initial_sync);
    xSemaphoreGive(button->lock);
    return result;
}

/**
 * @brief Advance one binding through debounce, long-press, and repeat deadlines.
 */
static void button_advance_binding(espio_button_t* button, button_binding_runtime_t* binding, uint64_t now_us) {
    if (!button || !binding) {
        return;
    }

    if (binding->debounce_pending && now_us >= binding->debounce_due_us) {
        if (binding->stable_active != binding->pending_active) {
            button_commit_stable_state(button, binding, binding->pending_active, binding->debounce_due_us);
        } else {
            binding->debounce_pending = false;
            binding->debounce_due_us = BUTTON_TIME_NONE;
        }
    }

    if (binding->stable_active &&
        !binding->snapshot.long_press_reported &&
        binding->config.behavior.long_press_ms > 0U &&
        binding->config.behavior.emit_long_press) {
        uint64_t long_press_due_us = binding->pressed_since_us +
                                     ((uint64_t)binding->config.behavior.long_press_ms * 1000ULL);
        if (now_us >= long_press_due_us) {
            binding->snapshot.long_press_reported = true;
            button_emit_event(button, binding, ESPIO_BUTTON_EVENT_LONG_PRESS, ESPIO_BUTTON_PRESSED, long_press_due_us);
        }
    }

    if (binding->stable_active &&
        binding->next_repeat_us != BUTTON_TIME_NONE &&
        now_us >= binding->next_repeat_us) {
        button_emit_event(button, binding, ESPIO_BUTTON_EVENT_REPEAT, ESPIO_BUTTON_PRESSED, binding->next_repeat_us);
        binding->next_repeat_us += ((uint64_t)binding->config.behavior.repeat_interval_ms * 1000ULL);
    }
}

/**
 * @brief Return the next scheduled deadline for provider polling or semantic timers.
 */
static uint64_t button_next_deadline_us(const espio_button_t* button) {
    uint64_t deadline_us = BUTTON_TIME_NONE;

    if (!button) {
        return BUTTON_TIME_NONE;
    }

    for (size_t provider_index = 0; provider_index < button->provider_count; provider_index++) {
        const button_provider_runtime_t* provider = &button->providers[provider_index];
        if (provider->poll_period_ms > 0U && provider->next_poll_due_us < deadline_us) {
            deadline_us = provider->next_poll_due_us;
        }
    }

    for (size_t binding_index = 0; binding_index < button->binding_count; binding_index++) {
        const button_binding_runtime_t* binding = &button->bindings[binding_index];
        if (binding->debounce_pending && binding->debounce_due_us < deadline_us) {
            deadline_us = binding->debounce_due_us;
        }

        if (binding->stable_active &&
            !binding->snapshot.long_press_reported &&
            binding->config.behavior.long_press_ms > 0U &&
            binding->config.behavior.emit_long_press) {
            uint64_t long_press_due_us = binding->pressed_since_us +
                                         ((uint64_t)binding->config.behavior.long_press_ms * 1000ULL);
            if (long_press_due_us < deadline_us) {
                deadline_us = long_press_due_us;
            }
        }

        if (binding->stable_active &&
            binding->next_repeat_us != BUTTON_TIME_NONE &&
            binding->next_repeat_us < deadline_us) {
            deadline_us = binding->next_repeat_us;
        }
    }

    return deadline_us;
}

/**
 * @brief Convert a microsecond deadline into an RTOS wait time.
 */
static TickType_t button_deadline_to_ticks(uint64_t deadline_us) {
    uint64_t now_us;
    uint64_t remaining_us;
    uint64_t ticks;

    if (deadline_us == BUTTON_TIME_NONE) {
        return portMAX_DELAY;
    }

    now_us = button_now_us(NULL);
    if (deadline_us <= now_us) {
        return 0U;
    }

    remaining_us = deadline_us - now_us;
    ticks = (remaining_us + 999ULL) / 1000ULL;
    ticks = pdMS_TO_TICKS(ticks);
    if (ticks == 0ULL) {
        return 1U;
    }

    if (ticks > (uint64_t)portMAX_DELAY) {
        return portMAX_DELAY;
    }

    return (TickType_t)ticks;
}

/**
 * @brief Main worker task that collects providers and advances semantic timers.
 */
static void button_task_main(void* arg) {
    espio_button_t* button = (espio_button_t*)arg;

    while (button && !button->stop_requested) {
        uint32_t notified_bits = 0U;
        uint64_t now_us = 0U;
        uint64_t deadline_us = button_next_deadline_us(button);
        TickType_t wait_ticks = button_deadline_to_ticks(deadline_us);

        xTaskNotifyWait(0U, UINT32_MAX, &notified_bits, wait_ticks);
        if ((notified_bits & BUTTON_NOTIFY_STOP_BIT) != 0U) {
            break;
        }

        now_us = button_now_us(NULL);

        for (size_t provider_index = 0; provider_index < button->provider_count; provider_index++) {
            button_provider_runtime_t* provider = &button->providers[provider_index];
            bool provider_notified = (notified_bits & (1UL << provider_index)) != 0U;
            bool provider_due = provider->poll_period_ms > 0U && now_us >= provider->next_poll_due_us;

            if (provider_notified) {
                button_collect_provider(button, provider, ESPIO_BUTTON_COLLECT_INTERRUPT, false);
            }

            if (provider_due) {
                button_collect_provider(button, provider, ESPIO_BUTTON_COLLECT_POLL, false);
                provider->next_poll_due_us = now_us + ((uint64_t)provider->poll_period_ms * 1000ULL);
            }
        }

        if (xSemaphoreTake(button->lock, portMAX_DELAY) == pdTRUE) {
            for (size_t binding_index = 0; binding_index < button->binding_count; binding_index++) {
                button_advance_binding(button, &button->bindings[binding_index], now_us);
            }
            xSemaphoreGive(button->lock);
        }
    }

    if (button) {
        button->task_stopped = true;
    }
    vTaskDelete(NULL);
}

/**
 * @brief Free all heap allocations owned by one binding runtime.
 */
static void button_free_binding_runtime(button_binding_runtime_t* binding) {
    if (!binding) {
        return;
    }

    heap_caps_free((void*)binding->config.sources);
    heap_caps_free(binding->source_indices);
    memset(binding, 0, sizeof(*binding));
}

/**
 * @brief Destroy provider runtimes and generic heap allocations.
 */
static void button_free_runtime(espio_button_t* button) {
    if (!button) {
        return;
    }

    if (button->providers) {
        for (size_t provider_index = 0; provider_index < button->provider_count; provider_index++) {
            button_provider_runtime_t* provider = &button->providers[provider_index];
            if (provider->driver && provider->driver->stop && provider->driver_ctx) {
                provider->driver->stop(provider->driver_ctx);
            }
            if (provider->driver && provider->driver->deinit && provider->driver_ctx) {
                provider->driver->deinit(provider->driver_ctx);
            }
        }
    }

    if (button->bindings) {
        for (size_t binding_index = 0; binding_index < button->binding_count; binding_index++) {
            button_free_binding_runtime(&button->bindings[binding_index]);
        }
    }

    if (button->event_queue) {
        vQueueDelete(button->event_queue);
    }
    if (button->lock) {
        vSemaphoreDelete(button->lock);
    }

    heap_caps_free(button->scratch_changes);
    heap_caps_free(button->sources);
    heap_caps_free(button->bindings);
    heap_caps_free(button->providers);
    heap_caps_free(button);
}

/**
 * @brief Deep-copy binding arrays and build the unique source cache used by the manager.
 */
static int32_t button_copy_bindings(espio_button_t* button, const espio_button_config_t* config) {
    size_t next_source_index = 0U;

    if (!button || !config) {
        return ESPIO_INVALID_ARG;
    }

    button->bindings = heap_caps_calloc(config->binding_count, sizeof(button_binding_runtime_t), MALLOC_CAP_DEFAULT);
    if (!button->bindings) {
        return ESPIO_NO_MEM;
    }

    for (size_t binding_index = 0; binding_index < config->binding_count; binding_index++) {
        const espio_button_binding_t* src_binding = &config->bindings[binding_index];
        button_binding_runtime_t* dst_binding = &button->bindings[binding_index];

        dst_binding->config = *src_binding;
        if (src_binding->use_service_defaults) {
            button_apply_behavior_defaults(&dst_binding->config.behavior, &button->service);
        }
        dst_binding->config.sources = heap_caps_calloc(src_binding->source_count,
                                                       sizeof(espio_button_source_ref_t),
                                                       MALLOC_CAP_DEFAULT);
        dst_binding->source_indices = heap_caps_calloc(src_binding->source_count,
                                                       sizeof(size_t),
                                                       MALLOC_CAP_DEFAULT);
        if ((!dst_binding->config.sources && src_binding->source_count > 0U) ||
            (!dst_binding->source_indices && src_binding->source_count > 0U)) {
            return ESPIO_NO_MEM;
        }

        memcpy((void*)dst_binding->config.sources,
               src_binding->sources,
               src_binding->source_count * sizeof(espio_button_source_ref_t));

        dst_binding->config.source_count = src_binding->source_count;
        dst_binding->debounce_due_us = BUTTON_TIME_NONE;
        dst_binding->pressed_since_us = BUTTON_TIME_NONE;
        dst_binding->next_repeat_us = BUTTON_TIME_NONE;

        for (size_t source_ref_index = 0; source_ref_index < src_binding->source_count; source_ref_index++) {
            size_t source_cache_index = 0U;
            int32_t result = button_get_or_add_source(button,
                                                      &src_binding->sources[source_ref_index],
                                                      next_source_index,
                                                      &source_cache_index);
            if (result != ESPIO_BUTTON_OK) {
                return result;
            }

            if (source_cache_index == next_source_index) {
                next_source_index++;
            }
            dst_binding->source_indices[source_ref_index] = source_cache_index;
        }
    }

    button->source_count = next_source_index;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Validate top-level API configuration before heap allocations begin.
 */
static int32_t button_validate_config(const espio_button_config_t* config) {
    if (!config || !config->providers || !config->bindings) {
        return ESPIO_INVALID_ARG;
    }

    if (config->provider_count == 0U || config->binding_count == 0U) {
        return ESPIO_INVALID_ARG;
    }

    if (config->provider_count > BUTTON_MAX_PROVIDER_COUNT) {
        return ESPIO_UNSUPPORTED;
    }

    for (size_t provider_index = 0; provider_index < config->provider_count; provider_index++) {
        const espio_button_provider_config_t* provider = &config->providers[provider_index];
        if (!provider->driver ||
            !provider->driver->query_caps ||
            !provider->driver->init ||
            !provider->driver->collect) {
            return ESPIO_INVALID_ARG;
        }

        for (size_t other_index = provider_index + 1U; other_index < config->provider_count; other_index++) {
            if (provider->provider_id == config->providers[other_index].provider_id) {
                return ESPIO_INVALID_ARG;
            }
        }
    }

    for (size_t binding_index = 0; binding_index < config->binding_count; binding_index++) {
        const espio_button_binding_t* binding = &config->bindings[binding_index];
        if (!binding->sources || binding->source_count == 0U) {
            return ESPIO_INVALID_ARG;
        }

        if (binding->mode != ESPIO_BUTTON_BIND_ANY &&
            binding->mode != ESPIO_BUTTON_BIND_ALL) {
            return ESPIO_UNSUPPORTED;
        }

        for (size_t other_binding_index = binding_index + 1U;
             other_binding_index < config->binding_count;
             other_binding_index++) {
            if (binding->button_id == config->bindings[other_binding_index].button_id) {
                return ESPIO_INVALID_ARG;
            }
        }

        for (size_t source_index = 0; source_index < binding->source_count; source_index++) {
            bool provider_found = false;

            for (size_t provider_index = 0; provider_index < config->provider_count; provider_index++) {
                if (binding->sources[source_index].provider_id == config->providers[provider_index].provider_id) {
                    provider_found = true;
                    break;
                }
            }

            if (!provider_found) {
                return ESPIO_NOT_FOUND;
            }
        }
    }

    return ESPIO_BUTTON_OK;
}

/**
 * @brief Create and initialize a button manager facade.
 */
int32_t espio_button_create(espio_button_t** out_button, const espio_button_config_t* config) {
    espio_button_t* button = NULL;
    espio_button_driver_host_t host = {0};
    int32_t result = ESPIO_BUTTON_OK;
    size_t unique_source_capacity = 0U;

    if (!out_button) {
        return ESPIO_INVALID_ARG;
    }
    *out_button = NULL;

    result = button_validate_config(config);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    button = heap_caps_calloc(1, sizeof(espio_button_t), MALLOC_CAP_DEFAULT);
    if (!button) {
        return ESPIO_NO_MEM;
    }

    button->service = config->service;
    if (button->service.queue_depth == 0U) {
        button->service.queue_depth = ESPIO_BUTTON_SERVICE_CONFIG_DEFAULT().queue_depth;
    }
    if (button->service.task_stack_size == 0U) {
        button->service.task_stack_size = ESPIO_BUTTON_SERVICE_CONFIG_DEFAULT().task_stack_size;
    }
    if (button->service.task_priority == 0U) {
        button->service.task_priority = ESPIO_BUTTON_SERVICE_CONFIG_DEFAULT().task_priority;
    }
    if (button->service.default_poll_period_ms == 0U) {
        button->service.default_poll_period_ms = ESPIO_BUTTON_SERVICE_CONFIG_DEFAULT().default_poll_period_ms;
    }
    button->service.default_behavior = config->service.default_behavior;

    button->provider_count = config->provider_count;
    button->binding_count = config->binding_count;
    button->last_error = ESPIO_BUTTON_OK;

    unique_source_capacity = 0U;
    for (size_t binding_index = 0; binding_index < config->binding_count; binding_index++) {
        unique_source_capacity += config->bindings[binding_index].source_count;
    }

    button->providers = heap_caps_calloc(button->provider_count,
                                         sizeof(button_provider_runtime_t),
                                         MALLOC_CAP_DEFAULT);
    button->sources = heap_caps_calloc(unique_source_capacity,
                                       sizeof(button_source_cache_entry_t),
                                       MALLOC_CAP_DEFAULT);
    if (!button->providers || !button->sources) {
        button_free_runtime(button);
        return ESPIO_NO_MEM;
    }

    button->lock = xSemaphoreCreateMutex();
    if (!button->lock) {
        button_free_runtime(button);
        return ESPIO_NO_MEM;
    }

    result = button_copy_bindings(button, config);
    if (result != ESPIO_BUTTON_OK) {
        button_free_runtime(button);
        return result;
    }

    host.context = button;
    host.notify = button_host_notify;
    host.notify_from_isr = button_host_notify_from_isr;
    host.get_time_us = button_now_us;

    for (size_t provider_index = 0; provider_index < button->provider_count; provider_index++) {
        const espio_button_provider_config_t* provider_config = &config->providers[provider_index];
        button_provider_runtime_t* provider = &button->providers[provider_index];

        provider->provider_id = provider_config->provider_id;
        provider->driver = provider_config->driver;
        provider->poll_period_ms = provider_config->poll_period_ms;
        if (provider->poll_period_ms == 0U) {
            provider->poll_period_ms = button->service.default_poll_period_ms;
        }

        result = provider->driver->query_caps(provider_config->driver_config, &provider->caps);
        if (result != ESPIO_BUTTON_OK) {
            button_free_runtime(button);
            return result;
        }

        if (provider->caps.notification != ESPIO_BUTTON_NOTIFY_POLL && provider_config->poll_period_ms == 0U) {
            provider->poll_period_ms = 0U;
        }

        if (provider->caps.max_events_per_collect > button->scratch_capacity) {
            button->scratch_capacity = provider->caps.max_events_per_collect;
        }

        provider->driver_ctx = provider->driver->init(provider_config->driver_config, &host, (uint32_t)provider_index);
        if (!provider->driver_ctx) {
            button_free_runtime(button);
            return ESPIO_BUTTON_ERR_DRIVER;
        }
    }

    for (size_t source_index = 0; source_index < button->source_count; source_index++) {
        if (!button_find_provider(button, button->sources[source_index].provider_id)) {
            button_free_runtime(button);
            return ESPIO_NOT_FOUND;
        }
    }

    if (button->scratch_capacity == 0U) {
        button->scratch_capacity = 1U;
    }

    button->scratch_changes = heap_caps_calloc(button->scratch_capacity,
                                               sizeof(espio_button_physical_change_t),
                                               MALLOC_CAP_DEFAULT);
    if (!button->scratch_changes) {
        button_free_runtime(button);
        return ESPIO_NO_MEM;
    }

    for (size_t provider_index = 0; provider_index < button->provider_count; provider_index++) {
        result = button_collect_provider(button,
                                         &button->providers[provider_index],
                                         ESPIO_BUTTON_COLLECT_SYNC,
                                         true);
        if (result != ESPIO_BUTTON_OK) {
            button_free_runtime(button);
            return result;
        }
    }

    button->event_queue = xQueueCreate(button->service.queue_depth, sizeof(espio_button_event_t));
    if (!button->event_queue) {
        button_free_runtime(button);
        return ESPIO_NO_MEM;
    }

    if (xTaskCreate(button_task_main,
                    "button_task",
                    button->service.task_stack_size,
                    button,
                    (UBaseType_t)button->service.task_priority,
                    &button->task_handle) != pdPASS) {
        button_free_runtime(button);
        return ESPIO_NO_MEM;
    }

    for (size_t provider_index = 0; provider_index < button->provider_count; provider_index++) {
        button_provider_runtime_t* provider = &button->providers[provider_index];
        provider->next_poll_due_us = button_now_us(NULL) + ((uint64_t)provider->poll_period_ms * 1000ULL);
        if (provider->driver->start) {
            result = provider->driver->start(provider->driver_ctx);
            if (result != ESPIO_BUTTON_OK) {
                espio_button_destroy(button);
                return result;
            }
        }
    }

    *out_button = button;
    ESPIO_LOGI(TAG, "button manager created with %u providers and %u bindings",
               (unsigned)button->provider_count,
               (unsigned)button->binding_count);
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Destroy a button manager facade.
 */
void espio_button_destroy(espio_button_t* button) {
    if (!button) {
        return;
    }

    button->stop_requested = true;
    if (button->task_handle) {
        xTaskNotify(button->task_handle, BUTTON_NOTIFY_STOP_BIT, eSetBits);
        for (uint32_t attempt = 0; attempt < 50U && !button->task_stopped; attempt++) {
            vTaskDelay(pdMS_TO_TICKS(1U));
        }
        if (!button->task_stopped) {
            vTaskDelete(button->task_handle);
        }
    }

    button_free_runtime(button);
}

/**
 * @brief Wait for and receive one semantic button event.
 */
int32_t espio_button_get_event(espio_button_t* button, espio_button_event_t* out_event, uint32_t timeout_ms) {
    TickType_t wait_ticks;

    if (!button || !out_event) {
        return ESPIO_INVALID_ARG;
    }

    wait_ticks = timeout_ms == ESPIO_BUTTON_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(button->event_queue, out_event, wait_ticks) != pdTRUE) {
        return ESPIO_TIMEOUT;
    }

    return ESPIO_BUTTON_OK;
}

/**
 * @brief Find the runtime binding that owns one logical button identifier.
 */
static button_binding_runtime_t* button_find_binding(espio_button_t* button, espio_button_id_t button_id) {
    if (!button) {
        return NULL;
    }

    for (size_t binding_index = 0; binding_index < button->binding_count; binding_index++) {
        if (button->bindings[binding_index].config.button_id == button_id) {
            return &button->bindings[binding_index];
        }
    }

    return NULL;
}

/**
 * @brief Query the current stable state of one logical button.
 */
int32_t espio_button_get_state(const espio_button_t* button, espio_button_id_t button_id, espio_button_state_snapshot_t* out_state) {
    button_binding_runtime_t* binding;

    if (!button || !out_state) {
        return ESPIO_INVALID_ARG;
    }

    if (xSemaphoreTake(button->lock, portMAX_DELAY) != pdTRUE) {
        return ESPIO_BUSY;
    }

    binding = button_find_binding((espio_button_t*)button, button_id);
    if (!binding) {
        xSemaphoreGive(button->lock);
        return ESPIO_NOT_FOUND;
    }

    *out_state = binding->snapshot;
    xSemaphoreGive(button->lock);
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Query the time of the last semantic event emitted for one logical button.
 */
int32_t espio_button_get_last_event_time(const espio_button_t* button, espio_button_id_t button_id, uint64_t* out_timestamp_us) {
    espio_button_state_snapshot_t snapshot;
    int32_t result;

    if (!out_timestamp_us) {
        return ESPIO_INVALID_ARG;
    }

    result = espio_button_get_state(button, button_id, &snapshot);
    if (result != ESPIO_BUTTON_OK) {
        return result;
    }

    *out_timestamp_us = snapshot.last_event_time_us;
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Query the dropped-event counter maintained by the manager.
 */
int32_t espio_button_get_dropped_event_count(const espio_button_t* button, uint32_t* out_count) {
    if (!button || !out_count) {
        return ESPIO_INVALID_ARG;
    }

    if (xSemaphoreTake(button->lock, portMAX_DELAY) != pdTRUE) {
        return ESPIO_BUSY;
    }

    *out_count = button->dropped_event_count;
    xSemaphoreGive(button->lock);
    return ESPIO_BUTTON_OK;
}

/**
 * @brief Get the last public error recorded by the manager.
 */
int32_t espio_button_get_last_error(const espio_button_t* button) {
    int32_t last_error;

    if (!button) {
        return ESPIO_INVALID_ARG;
    }

    if (xSemaphoreTake(button->lock, portMAX_DELAY) != pdTRUE) {
        return ESPIO_BUSY;
    }

    last_error = button->last_error;
    xSemaphoreGive(button->lock);
    return last_error;
}

/**
 * @brief Convert one public error code into a stable string.
 */
const char* espio_button_err_to_name(int32_t error) {
    switch (error) {
        case ESPIO_BUTTON_OK:
            return "ESPIO_BUTTON_OK";
        case ESPIO_INVALID_ARG:
            return "ESPIO_INVALID_ARG";
        case ESPIO_NO_MEM:
            return "ESPIO_NO_MEM";
        case ESPIO_NOT_FOUND:
            return "ESPIO_NOT_FOUND";
        case ESPIO_TIMEOUT:
            return "ESPIO_TIMEOUT";
        case ESPIO_BUSY:
            return "ESPIO_BUSY";
        case ESPIO_UNSUPPORTED:
            return "ESPIO_UNSUPPORTED";
        case ESPIO_BUTTON_ERR_QUEUE_FULL:
            return "ESPIO_BUTTON_ERR_QUEUE_FULL";
        case ESPIO_IO:
            return "ESPIO_IO";
        case ESPIO_BUTTON_ERR_DRIVER:
            return "ESPIO_BUTTON_ERR_DRIVER";
        case ESPIO_INTERNAL:
            return "ESPIO_INTERNAL";
        default:
            return "ESPIO_BUTTON_ERR_UNRECOGNIZED";
    }
}
