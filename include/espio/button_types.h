/**
 * @file button_types.h
 * @brief Shared public types for the button abstraction API.
 */

#ifndef ESPIO_BUTTON_TYPES_H
#define ESPIO_BUTTON_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "espio/button_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Public sentinel for an unused or absent pin.
 */
#define ESPIO_BUTTON_PIN_UNUSED (-1)

/**
 * @brief Public timeout value that waits indefinitely.
 */
#define ESPIO_BUTTON_WAIT_FOREVER UINT32_MAX

/**
 * @brief Stable identifier of one logical button exposed to the application.
 */
typedef uint16_t espio_button_id_t;

/**
 * @brief Stable identifier of one provider-local physical channel.
 */
typedef uint16_t espio_button_channel_id_t;

/**
 * @brief Public logical state of one button-like control.
 */
typedef enum {
    ESPIO_BUTTON_RELEASED = 0,  ///< The logical control is inactive.
    ESPIO_BUTTON_PRESSED = 1    ///< The logical control is active.
} espio_button_state_t;

/**
 * @brief Public event types emitted by the button service.
 */
typedef enum {
    ESPIO_BUTTON_EVENT_PRESS = 0,      ///< Stable logical press transition.
    ESPIO_BUTTON_EVENT_RELEASE = 1,    ///< Stable logical release transition.
    ESPIO_BUTTON_EVENT_LONG_PRESS = 2, ///< Long-press threshold was reached.
    ESPIO_BUTTON_EVENT_REPEAT = 3      ///< Repeat interval fired while the button stayed pressed.
} espio_button_event_type_t;

/**
 * @brief Public source families supported by provider drivers.
 */
typedef enum {
    ESPIO_BUTTON_SOURCE_GPIO = 0,      ///< Direct GPIO-backed inputs.
    ESPIO_BUTTON_SOURCE_MATRIX = 1,    ///< MCU-scanned or controller-scanned matrix inputs.
    ESPIO_BUTTON_SOURCE_EXPANDER = 2,  ///< Off-chip digital frontend or port expander.
    ESPIO_BUTTON_SOURCE_TOUCH = 3,     ///< Capacitive or touch-derived sensing.
    ESPIO_BUTTON_SOURCE_ADC = 4,       ///< ADC ladder or threshold-based analog input.
    ESPIO_BUTTON_SOURCE_ENCODER = 5,   ///< Relative input source such as a rotary encoder.
    ESPIO_BUTTON_SOURCE_VIRTUAL = 6    ///< Remote or software-originated logical source.
} espio_button_source_family_t;

/**
 * @brief Notification style supported by a provider.
 */
typedef enum {
    ESPIO_BUTTON_NOTIFY_POLL = 0,       ///< The manager must poll the provider periodically.
    ESPIO_BUTTON_NOTIFY_INTERRUPT = 1,  ///< The provider can wake the manager, but still requires collection.
    ESPIO_BUTTON_NOTIFY_BUFFERED = 2    ///< The provider buffers changes and reports them during collection.
} espio_button_notification_mode_t;

/**
 * @brief Reason why the manager asks a provider to collect physical changes.
 */
typedef enum {
    ESPIO_BUTTON_COLLECT_POLL = 0,       ///< Periodic polling cycle.
    ESPIO_BUTTON_COLLECT_INTERRUPT = 1,  ///< Wake-up caused by an interrupt hint.
    ESPIO_BUTTON_COLLECT_SYNC = 2,       ///< Initial synchronization without semantic event emission.
    ESPIO_BUTTON_COLLECT_SHUTDOWN = 3    ///< Final drain before provider shutdown.
} espio_button_collect_reason_t;

/**
 * @brief How multiple sources are combined into one logical button.
 */
typedef enum {
    ESPIO_BUTTON_BIND_ANY = 0,  ///< The binding is pressed when any source is active.
    ESPIO_BUTTON_BIND_ALL = 1   ///< The binding is pressed only when all sources are active.
} espio_button_binding_mode_t;

/**
 * @brief One physical source reference used by a logical binding.
 */
typedef struct {
    uint32_t provider_id;                  ///< Stable provider identifier from the manager configuration.
    espio_button_channel_id_t channel_id;  ///< Provider-local physical channel identifier.
} espio_button_source_ref_t;

/**
 * @brief Per-binding timing and emission policy.
 */
typedef struct {
    uint32_t debounce_ms;                  ///< Debounce window applied to raw aggregate state changes.
    uint32_t long_press_ms;                ///< Long-press threshold, or zero to disable long-press events.
    uint32_t repeat_delay_ms;              ///< Delay before the first repeat event, or zero to disable repeats.
    uint32_t repeat_interval_ms;           ///< Interval between repeat events after the first repeat.
    bool emit_press;                       ///< Emit `BUTTON_EVENT_PRESS` when a stable press is committed.
    bool emit_release;                     ///< Emit `BUTTON_EVENT_RELEASE` when a stable release is committed.
    bool emit_long_press;                  ///< Emit `BUTTON_EVENT_LONG_PRESS` when enabled.
    bool emit_repeat;                      ///< Emit `BUTTON_EVENT_REPEAT` when enabled.
} espio_button_behavior_t;

/**
 * @brief Convenience defaults for binding behavior.
 */
static inline espio_button_behavior_t espio_button_behavior_default(void) {
    espio_button_behavior_t config = {
        .debounce_ms = 30U,
        .long_press_ms = 500U,
        .repeat_delay_ms = 0U,
        .repeat_interval_ms = 0U,
        .emit_press = true,
        .emit_release = true,
        .emit_long_press = true,
        .emit_repeat = true
    };
    return config;
}
#define ESPIO_BUTTON_BEHAVIOR_DEFAULT() espio_button_behavior_default()

/**
 * @brief One logical binding exposed to the application.
 */
typedef struct {
    espio_button_id_t button_id;           ///< Stable logical button identifier.
    espio_button_binding_mode_t mode;      ///< Source aggregation policy.
    const espio_button_source_ref_t* sources; ///< Source references used by this binding.
    size_t source_count;                   ///< Number of source references.
    bool use_service_defaults;             ///< True when the manager should replace `behavior` with the service default.
    espio_button_behavior_t behavior;      ///< Timing and emission policy for this binding.
} espio_button_binding_t;

/**
 * @brief Public event delivered by the button manager.
 */
typedef struct {
    espio_button_id_t button_id;           ///< Logical button that generated the event.
    espio_button_event_type_t type;        ///< Semantic event type.
    espio_button_state_t state;            ///< Stable state after the event was applied.
    uint64_t timestamp_us;                 ///< Monotonic timestamp in microseconds.
    uint32_t sequence;                     ///< Per-manager event sequence number.
} espio_button_event_t;

/**
 * @brief Public state snapshot returned by query functions.
 */
typedef struct {
    espio_button_state_t state;            ///< Current stable logical state.
    bool long_press_reported;              ///< True after the current press already produced a long-press event.
    uint64_t last_change_time_us;          ///< Monotonic time of the last stable state change.
    uint64_t last_event_time_us;           ///< Monotonic time of the last emitted semantic event.
} espio_button_state_snapshot_t;

/**
 * @brief Generic service-level configuration.
 */
typedef struct {
    size_t queue_depth;                    ///< Queue capacity for semantic events.
    uint32_t task_stack_size;              ///< Worker task stack size in bytes.
    uint32_t task_priority;                ///< Worker task priority.
    uint32_t default_poll_period_ms;       ///< Fallback poll period for providers that do not override it.
    espio_button_behavior_t default_behavior; ///< Default behavior copied into bindings that set `use_service_defaults`.
} espio_button_service_config_t;

/**
 * @brief Convenience defaults for service configuration.
 */
static inline espio_button_service_config_t espio_button_service_config_default(void) {
    espio_button_service_config_t config = {
        .queue_depth = 16U,
        .task_stack_size = 4096U,
        .task_priority = 5U,
        .default_poll_period_ms = 20U,
        .default_behavior = ESPIO_BUTTON_BEHAVIOR_DEFAULT()
    };
    return config;
}
#define ESPIO_BUTTON_SERVICE_CONFIG_DEFAULT() espio_button_service_config_default()

/**
 * @brief Capabilities reported by one provider driver.
 */
typedef struct {
    espio_button_source_family_t source_family;    ///< Family of the provider implementation.
    espio_button_notification_mode_t notification; ///< How the manager should wake or poll the provider.
    size_t channel_count;                      ///< Number of physical channels owned by the provider.
    size_t max_events_per_collect;             ///< Maximum number of physical changes returned in one collection pass.
    bool supports_hardware_debounce;           ///< True when the provider or hardware already debounces changes.
    bool supports_simultaneous_inputs;         ///< True when several channels may be active together.
    bool supports_relative_events;             ///< True for providers that emit relative events instead of binary states.
    bool supports_wakeup_hint;                 ///< True when the provider can wake the manager asynchronously.
} espio_button_driver_caps_t;

/**
 * @brief One physical change reported by a provider during collection.
 */
typedef struct {
    espio_button_channel_id_t channel_id;  ///< Provider-local channel identifier.
    bool active;                           ///< True when the physical source is currently active.
    uint64_t timestamp_us;                 ///< Monotonic time observed by the provider, or zero to use manager time.
} espio_button_physical_change_t;

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_BUTTON_TYPES_H */
