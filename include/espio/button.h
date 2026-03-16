/**
 * @file button.h
 * @brief Generic button abstraction facade and driver contract.
 */

#ifndef ESPIO_BUTTON_H
#define ESPIO_BUTTON_H

#include "espio/button_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque button manager handle.
 */
typedef struct espio_button espio_button_t;

/**
 * @brief Opaque provider runtime handle used by driver implementations.
 */
typedef struct espio_button_driver_host {
    void* context;                                                     ///< Host-owned opaque context pointer.
    void (*notify)(void* context, uint32_t provider_index);            ///< Wake the manager from task context.
    void (*notify_from_isr)(void* context, uint32_t provider_index, bool* higher_priority_task_woken); ///< Wake the manager from ISR context.
    uint64_t (*get_time_us)(void* context);                            ///< Return the current monotonic time in microseconds.
} espio_button_driver_host_t;

/**
 * @brief Concrete provider driver contract.
 */
typedef struct espio_button_driver {
    const char* name;   ///< Driver name used for logs and diagnostics.

    /**
     * @brief Query capabilities for one driver-specific configuration.
     * @param config Driver-specific configuration.
     * @param caps Output capability structure.
     * @return BUTTON_OK on success or a public error code on failure.
     */
    int32_t (*query_caps)(const void* config, espio_button_driver_caps_t* caps);

    /**
     * @brief Initialize driver-specific runtime state.
     * @param config Driver-specific configuration.
     * @param host Manager host callbacks used by the provider.
     * @param provider_index Zero-based internal provider index owned by the manager.
     * @return Driver context on success or NULL on failure.
     */
    void* (*init)(const void* config, const espio_button_driver_host_t* host, uint32_t provider_index);

    /**
     * @brief Deinitialize driver-specific runtime state.
     * @param ctx Driver context.
     */
    void (*deinit)(void* ctx);

    /**
     * @brief Start provider activity after the manager is fully constructed.
     * @param ctx Driver context.
     * @return BUTTON_OK on success or a public error code on failure.
     */
    int32_t (*start)(void* ctx);

    /**
     * @brief Stop provider activity before manager destruction.
     * @param ctx Driver context.
     */
    void (*stop)(void* ctx);

    /**
     * @brief Collect physical changes since the previous collection pass.
     * @param ctx Driver context.
     * @param reason Reason for the collection call.
     * @param changes Output buffer for physical changes.
     * @param capacity Capacity of the output buffer in change items.
     * @param out_count Output number of valid items in the buffer.
     * @return BUTTON_OK on success or a public error code on failure.
     */
    int32_t (*collect)(void* ctx,
                       espio_button_collect_reason_t reason,
                       espio_button_physical_change_t* changes,
                       size_t capacity,
                       size_t* out_count);
} espio_button_driver_t;

/**
 * @brief Generic provider registration entry used by the manager.
 */
typedef struct {
    uint32_t provider_id;                  ///< Stable provider identifier used by bindings.
    const espio_button_driver_t* driver;   ///< Concrete provider driver.
    const void* driver_config;             ///< Driver-specific configuration passed to the concrete driver.
    uint32_t poll_period_ms;               ///< Provider-specific poll period, or zero to use the service default.
} espio_button_provider_config_t;

/**
 * @brief Top-level manager configuration.
 */
typedef struct {
    const espio_button_provider_config_t* providers; ///< Provider registrations owned by the manager.
    size_t provider_count;                     ///< Number of provider registrations.
    const espio_button_binding_t* bindings;   ///< Logical bindings owned by the manager.
    size_t binding_count;                      ///< Number of logical bindings.
    espio_button_service_config_t service;     ///< Service-level queue and timing configuration.
} espio_button_config_t;

/**
 * @brief Create and initialize a button manager facade.
 * @param out_button Output location for the created manager handle.
 * @param config Top-level manager configuration.
 * @return BUTTON_OK on success or a public error code on failure.
 */
int32_t espio_button_create(espio_button_t** out_button, const espio_button_config_t* config);

/**
 * @brief Destroy a button manager facade.
 * @param button Manager handle.
 */
void espio_button_destroy(espio_button_t* button);

/**
 * @brief Wait for and receive one semantic button event.
 * @param button Manager handle.
 * @param out_event Output semantic event.
 * @param timeout_ms Wait timeout in milliseconds or `ESPIO_BUTTON_WAIT_FOREVER`.
 * @return `ESPIO_BUTTON_OK` on success, `ESPIO_TIMEOUT` on timeout, or another public error on failure.
 */
int32_t espio_button_get_event(espio_button_t* button, espio_button_event_t* out_event, uint32_t timeout_ms);

/**
 * @brief Query the current stable state of one logical button.
 * @param button Manager handle.
 * @param button_id Logical button identifier.
 * @param out_state Output state snapshot.
 * @return BUTTON_OK on success or a public error code on failure.
 */
int32_t espio_button_get_state(const espio_button_t* button, espio_button_id_t button_id, espio_button_state_snapshot_t* out_state);

/**
 * @brief Query the monotonic time of the last semantic event emitted for one logical button.
 * @param button Manager handle.
 * @param button_id Logical button identifier.
 * @param out_timestamp_us Output timestamp in microseconds.
 * @return BUTTON_OK on success or a public error code on failure.
 */
int32_t espio_button_get_last_event_time(const espio_button_t* button, espio_button_id_t button_id, uint64_t* out_timestamp_us);

/**
 * @brief Query how many semantic events were dropped because the event queue was full.
 * @param button Manager handle.
 * @param out_count Output dropped-event count.
 * @return BUTTON_OK on success or a public error code on failure.
 */
int32_t espio_button_get_dropped_event_count(const espio_button_t* button, uint32_t* out_count);

/**
 * @brief Get the last public error recorded by the manager.
 * @param button Manager handle.
 * @return Last public error or `ESPIO_INVALID_ARG` when the handle is NULL.
 */
int32_t espio_button_get_last_error(const espio_button_t* button);

/**
 * @brief Convert a public error code into a stable string.
 * @param error Public error code.
 * @return Stable error name.
 */
const char* espio_button_err_to_name(int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_BUTTON_H */
