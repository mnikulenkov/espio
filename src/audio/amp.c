/**
 * @file amp.c
 * @brief Generic audio amplifier facade.
 */

#include "amp_internal.h"
#include "esp_heap_caps.h"
#include "espio/log.h"

static const char* TAG = "audio";

/**
 * @brief Resolve the effective runtime volume.
 */
uint8_t espio_audio_internal_effective_volume(const espio_audio_t* audio) {
    if (!audio) {
        return 0;
    }

    if (!audio->powered || audio->muted) {
        return 0;
    }

    return audio->volume_percent;
}

/**
 * @brief Create and initialize an audio amplifier handle.
 */
espio_audio_t* espio_audio_create(const espio_audio_config_t* config) {
    if (!config || !config->driver || !config->driver->query_caps || !config->driver->init) {
        ESPIO_LOGE(TAG, "invalid audio configuration");
        return NULL;
    }

    espio_audio_t* audio = heap_caps_calloc(1, sizeof(espio_audio_t), MALLOC_CAP_DEFAULT);
    if (!audio) {
        ESPIO_LOGE(TAG, "failed to allocate audio handle");
        return NULL;
    }

    audio->driver = config->driver;
    audio->dac = config->dac;
    audio->powered = true;
    audio->muted = false;
    audio->volume_percent = 100;

    if (audio->driver->query_caps(config->driver_config, &audio->caps) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to query capabilities for %s", audio->driver->name);
        heap_caps_free(audio);
        return NULL;
    }

    if (audio->caps.input_class == ESPIO_AUDIO_INPUT_CLASS_ANALOG_WITH_CONTROL && !audio->dac) {
        ESPIO_LOGE(TAG, "analog-input amplifier requires a DAC dependency");
        heap_caps_free(audio);
        return NULL;
    }

    audio->software_volume_enabled = config->enable_software_volume && !audio->caps.supports_hardware_volume;
    if (audio->software_volume_enabled) {
        audio->caps.supports_software_volume = true;
    }

    audio->driver_ctx = audio->driver->init(config->driver_config);
    if (!audio->driver_ctx) {
        ESPIO_LOGE(TAG, "failed to initialize %s", audio->driver->name);
        heap_caps_free(audio);
        return NULL;
    }

    ESPIO_LOGI(TAG, "audio amplifier created: %s", audio->driver->name);
    return audio;
}

/**
 * @brief Destroy an amplifier handle.
 */
void espio_audio_destroy(espio_audio_t* audio) {
    if (!audio) {
        return;
    }

    if (audio->driver && audio->driver->deinit) {
        audio->driver->deinit(audio->driver_ctx);
    }

    heap_caps_free(audio);
}

/**
 * @brief Get amplifier capabilities.
 */
const espio_audio_caps_t* espio_audio_get_caps(const espio_audio_t* audio) {
    if (!audio) {
        return NULL;
    }

    return &audio->caps;
}

/**
 * @brief Set amplifier power state.
 */
int32_t espio_audio_set_power(espio_audio_t* audio, bool on) {
    if (!audio || !audio->driver || !audio->driver->set_power) {
        return ESPIO_INVALID_ARG;
    }

    if (audio->caps.power_control == ESPIO_AUDIO_POWER_CONTROL_NONE) {
        return ESPIO_UNSUPPORTED;
    }

    if (audio->driver->set_power(audio->driver_ctx, on) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to set power state for %s", audio->driver->name);
        return ESPIO_AUDIO_ERR_DRIVER_FAILED;
    }

    audio->powered = on;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Set amplifier mute state.
 */
int32_t espio_audio_set_mute(espio_audio_t* audio, bool mute) {
    if (!audio || !audio->driver || !audio->driver->set_mute) {
        return ESPIO_INVALID_ARG;
    }

    if (!audio->caps.supports_mute) {
        return ESPIO_UNSUPPORTED;
    }

    if (audio->driver->set_mute(audio->driver_ctx, mute) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "failed to set mute state for %s", audio->driver->name);
        return ESPIO_AUDIO_ERR_DRIVER_FAILED;
    }

    audio->muted = mute;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Set amplifier volume.
 */
int32_t espio_audio_set_volume(espio_audio_t* audio, uint8_t percent) {
    if (!audio || percent > 100) {
        return ESPIO_INVALID_ARG;
    }

    if (audio->caps.supports_hardware_volume) {
        if (!audio->driver || !audio->driver->set_volume) {
            ESPIO_LOGE(TAG, "driver %s reports hardware volume without an implementation", audio->driver->name);
            return ESPIO_AUDIO_ERR_DRIVER_FAILED;
        }

        if (audio->driver->set_volume(audio->driver_ctx, percent) != ESPIO_AUDIO_OK) {
            ESPIO_LOGE(TAG, "failed to set hardware volume for %s", audio->driver->name);
            return ESPIO_AUDIO_ERR_DRIVER_FAILED;
        }
    } else if (!audio->software_volume_enabled) {
        ESPIO_LOGE(TAG, "volume is unavailable for %s", audio->driver->name);
        return ESPIO_UNSUPPORTED;
    }

    audio->volume_percent = percent;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Get the current logical volume.
 */
uint8_t espio_audio_get_volume(const espio_audio_t* audio) {
    if (!audio) {
        return 0;
    }

    return audio->volume_percent;
}
