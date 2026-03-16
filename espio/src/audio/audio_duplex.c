/**
 * @file audio_duplex.c
 * @brief Full-duplex PCM transport wrapper.
 */

#include "audio_i2s_hal.h"
#include "esp_heap_caps.h"
#include "espio/audio_duplex.h"
#include "espio/log.h"

static const char* TAG = "audio_duplex";

/**
 * @brief Internal full-duplex transport state.
 */
struct espio_audio_duplex {
    audio_i2s_hal_t hal;               ///< Shared full-duplex I2S transport.
    espio_audio_format_t format;       ///< Shared PCM format.
    uint32_t read_timeout_ms;          ///< Default RX timeout.
    uint32_t write_timeout_ms;         ///< Default TX timeout.
    bool started;                      ///< True when both directions are enabled.
};

/**
 * @brief Create a full-duplex I2S transport.
 */
espio_audio_duplex_t* espio_audio_duplex_create(const espio_audio_duplex_config_t* config) {
    if (!config) {
        ESPIO_LOGE(TAG, "invalid duplex configuration");
        return NULL;
    }

    espio_audio_duplex_t* duplex = heap_caps_calloc(1, sizeof(espio_audio_duplex_t), MALLOC_CAP_DEFAULT);
    if (!duplex) {
        ESPIO_LOGE(TAG, "failed to allocate duplex transport");
        return NULL;
    }

    duplex->format = config->format;
    duplex->read_timeout_ms = config->read_timeout_ms;
    duplex->write_timeout_ms = config->write_timeout_ms;

    int32_t result = audio_i2s_hal_init_full_duplex(&duplex->hal,
                                                    &config->format,
                                                    &config->pins,
                                                    config->controller_id,
                                                    config->mclk_policy);
    if (result != ESPIO_AUDIO_OK) {
        heap_caps_free(duplex);
        return NULL;
    }

    ESPIO_LOGI(TAG, "duplex transport created");
    return duplex;
}

/**
 * @brief Destroy a full-duplex transport.
 */
void espio_audio_duplex_destroy(espio_audio_duplex_t* duplex) {
    if (!duplex) {
        return;
    }

    audio_i2s_hal_deinit(&duplex->hal);
    heap_caps_free(duplex);
}

/**
 * @brief Start both TX and RX directions.
 */
int32_t espio_audio_duplex_start(espio_audio_duplex_t* duplex) {
    if (!duplex) {
        return ESPIO_INVALID_ARG;
    }

    int32_t result = audio_i2s_hal_start_tx(&duplex->hal);
    if (result != ESPIO_AUDIO_OK) {
        return result;
    }

    result = audio_i2s_hal_start_rx(&duplex->hal);
    if (result != ESPIO_AUDIO_OK) {
        (void)audio_i2s_hal_stop_tx(&duplex->hal);
        return result;
    }

    duplex->started = true;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Stop both TX and RX directions.
 */
int32_t espio_audio_duplex_stop(espio_audio_duplex_t* duplex) {
    if (!duplex) {
        return ESPIO_INVALID_ARG;
    }

    int32_t tx_result = audio_i2s_hal_stop_tx(&duplex->hal);
    int32_t rx_result = audio_i2s_hal_stop_rx(&duplex->hal);
    duplex->started = false;

    if (tx_result != ESPIO_AUDIO_OK) {
        return tx_result;
    }
    return rx_result;
}

/**
 * @brief Read PCM data from the duplex RX side.
 */
int32_t espio_audio_duplex_read(espio_audio_duplex_t* duplex, void* samples, size_t size, size_t* bytes_read, uint32_t timeout_ms) {
    if (!duplex || !samples || size == 0U) {
        return ESPIO_INVALID_ARG;
    }
    if (!duplex->started) {
        return ESPIO_AUDIO_ERR_NOT_STARTED;
    }

    uint32_t effective_timeout_ms = timeout_ms == 0U ? duplex->read_timeout_ms : timeout_ms;
    return audio_i2s_hal_read(&duplex->hal, samples, size, bytes_read, effective_timeout_ms);
}

/**
 * @brief Write PCM data to the duplex TX side.
 */
int32_t espio_audio_duplex_write(espio_audio_duplex_t* duplex, const void* samples, size_t size, size_t* bytes_written, uint32_t timeout_ms) {
    if (!duplex || !samples || size == 0U) {
        return ESPIO_INVALID_ARG;
    }
    if (!duplex->started) {
        return ESPIO_AUDIO_ERR_NOT_STARTED;
    }

    uint32_t effective_timeout_ms = timeout_ms == 0U ? duplex->write_timeout_ms : timeout_ms;
    return audio_i2s_hal_write(&duplex->hal, samples, size, bytes_written, effective_timeout_ms);
}
