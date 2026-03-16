/**
 * @file mic.c
 * @brief Generic microphone facade.
 */

#include "mic_internal.h"
#include "esp_heap_caps.h"
#include "espio/log.h"
#include <math.h>

static const char* TAG = "audio_mic";

/**
 * @brief Convert a dB gain request into a linear multiplier.
 */
float espio_audio_mic_internal_db_to_linear(int8_t gain_db) {
    return powf(10.0f, (float)gain_db / 20.0f);
}

/**
 * @brief Resolve the base software gain configured on the microphone.
 */
float espio_audio_mic_internal_effective_gain_linear(const espio_audio_mic_t* mic) {
    if (!mic) {
        return 1.0f;
    }

    return mic->software_gain_enabled ? mic->software_gain_linear : 1.0f;
}

/**
 * @brief Create and initialize a microphone handle.
 */
espio_audio_mic_t* espio_audio_mic_create(const espio_audio_mic_config_t* config) {
    if (!config || !config->driver || !config->driver->query_caps || !config->driver->get_transport_config || !config->driver->init) {
        ESPIO_LOGE(TAG, "invalid microphone configuration");
        return NULL;
    }

    espio_audio_mic_t* mic = heap_caps_calloc(1, sizeof(espio_audio_mic_t), MALLOC_CAP_DEFAULT);
    if (!mic) {
        ESPIO_LOGE(TAG, "failed to allocate microphone handle");
        return NULL;
    }

    mic->driver = config->driver;
    mic->driver_config = config->driver_config;
    mic->powered = true;
    mic->gain_db = 0;
    mic->software_gain_linear = 1.0f;

    if (mic->driver->query_caps(config->driver_config, &mic->caps) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to query capabilities for %s", mic->driver->name);
        heap_caps_free(mic);
        return NULL;
    }

    if (mic->driver->get_transport_config(config->driver_config, &mic->transport_config) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to resolve transport for %s", mic->driver->name);
        heap_caps_free(mic);
        return NULL;
    }

    mic->software_gain_enabled = config->enable_software_gain && mic->caps.gain_type != ESPIO_AUDIO_MIC_GAIN_TYPE_HARDWARE;
    if (mic->software_gain_enabled) {
        mic->caps.gain_type = ESPIO_AUDIO_MIC_GAIN_TYPE_SOFTWARE;
    }

    mic->driver_ctx = mic->driver->init(config->driver_config);
    if (!mic->driver_ctx) {
        ESPIO_LOGE(TAG, "failed to initialize %s", mic->driver->name);
        heap_caps_free(mic);
        return NULL;
    }

    ESPIO_LOGI(TAG, "microphone created: %s", mic->driver->name);
    return mic;
}

/**
 * @brief Destroy a microphone handle.
 */
void espio_audio_mic_destroy(espio_audio_mic_t* mic) {
    if (!mic) {
        return;
    }

    if (mic->driver && mic->driver->deinit) {
        mic->driver->deinit(mic->driver_ctx);
    }

    heap_caps_free(mic);
}

/**
 * @brief Get microphone capabilities.
 */
const espio_audio_mic_caps_t* espio_audio_mic_get_caps(const espio_audio_mic_t* mic) {
    if (!mic) {
        return NULL;
    }

    return &mic->caps;
}

/**
 * @brief Get the microphone interface family.
 */
espio_audio_mic_interface_type_t espio_audio_mic_get_interface(const espio_audio_mic_t* mic) {
    if (!mic) {
        return ESPIO_AUDIO_MIC_INTERFACE_I2S;
    }

    return mic->caps.interface_type;
}

/**
 * @brief Set microphone power state.
 */
int32_t espio_audio_mic_set_power(espio_audio_mic_t* mic, bool on) {
    if (!mic || !mic->driver || !mic->driver->set_power) {
        return ESPIO_UNSUPPORTED;
    }

    int32_t result = mic->driver->set_power(mic->driver_ctx, on);
    if (result != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to set power state for %s", mic->driver->name);
        return result;
    }

    mic->powered = on;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Set microphone gain.
 */
int32_t espio_audio_mic_set_gain(espio_audio_mic_t* mic, int8_t gain_db) {
    if (!mic) {
        return ESPIO_INVALID_ARG;
    }

    if (gain_db < mic->caps.min_gain_db || gain_db > mic->caps.max_gain_db) {
        return ESPIO_INVALID_ARG;
    }

    if (mic->caps.gain_type == ESPIO_AUDIO_MIC_GAIN_TYPE_HARDWARE) {
        if (!mic->driver || !mic->driver->set_gain) {
            return ESPIO_UNSUPPORTED;
        }

        int32_t result = mic->driver->set_gain(mic->driver_ctx, gain_db);
        if (result != ESPIO_AUDIO_OK) {
            ESPIO_LOGE(TAG, "failed to set hardware gain for %s", mic->driver->name);
            return result;
        }
    } else if (!mic->software_gain_enabled) {
        return ESPIO_UNSUPPORTED;
    }

    mic->gain_db = gain_db;
    mic->software_gain_linear = espio_audio_mic_internal_db_to_linear(gain_db);
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Get the current microphone gain.
 */
int32_t espio_audio_mic_get_gain(const espio_audio_mic_t* mic, int8_t* gain_db) {
    if (!mic || !gain_db) {
        return ESPIO_INVALID_ARG;
    }

    if (mic->caps.gain_type == ESPIO_AUDIO_MIC_GAIN_TYPE_HARDWARE && mic->driver && mic->driver->get_gain) {
        return mic->driver->get_gain(mic->driver_ctx, gain_db);
    }

    *gain_db = mic->gain_db;
    return ESPIO_AUDIO_OK;
}
