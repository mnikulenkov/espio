/**
 * @file audio_i2s_hal.c
 * @brief ESP-IDF I2S standard-mode transport wrapper.
 */

#include "audio_i2s_hal.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_tdm.h"
#include "freertos/FreeRTOS.h"
#include "espio/log.h"
#include <string.h>

static const char* TAG = "audio_i2s";

/**
 * @brief Map public bit widths to ESP-IDF I2S bit-width enums.
 */
static int32_t audio_i2s_hal_map_bit_width(espio_audio_bit_width_t bit_width, i2s_data_bit_width_t* out_bit_width) {
    if (!out_bit_width) {
        return ESPIO_INVALID_ARG;
    }

    switch (bit_width) {
        case ESPIO_AUDIO_BIT_WIDTH_16:
            *out_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
            return ESPIO_AUDIO_OK;
        case ESPIO_AUDIO_BIT_WIDTH_24:
            *out_bit_width = I2S_DATA_BIT_WIDTH_24BIT;
            return ESPIO_AUDIO_OK;
        case ESPIO_AUDIO_BIT_WIDTH_32:
            *out_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
            return ESPIO_AUDIO_OK;
        default:
            return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }
}

/**
 * @brief Map public channel layout to ESP-IDF slot mode.
 */
static int32_t audio_i2s_hal_map_slot_mode(espio_audio_channel_mode_t channel_mode, i2s_slot_mode_t* out_slot_mode) {
    if (!out_slot_mode) {
        return ESPIO_INVALID_ARG;
    }

    switch (channel_mode) {
        case ESPIO_AUDIO_CHANNEL_MODE_MONO:
            *out_slot_mode = I2S_SLOT_MODE_MONO;
            return ESPIO_AUDIO_OK;
        case ESPIO_AUDIO_CHANNEL_MODE_STEREO:
            *out_slot_mode = I2S_SLOT_MODE_STEREO;
            return ESPIO_AUDIO_OK;
        default:
            return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }
}

/**
 * @brief Resolve the requested MCLK multiple.
 */
static int32_t audio_i2s_hal_apply_mclk_policy(espio_audio_mclk_policy_t policy, espio_audio_bit_width_t bit_width, i2s_std_clk_config_t* clk_cfg) {
    if (!clk_cfg) {
        return ESPIO_INVALID_ARG;
    }

    switch (policy) {
        case ESPIO_AUDIO_MCLK_POLICY_DISABLED:
            return ESPIO_AUDIO_OK;
        case ESPIO_AUDIO_MCLK_POLICY_AUTO:
            if (bit_width == ESPIO_AUDIO_BIT_WIDTH_24) {
                clk_cfg->mclk_multiple = I2S_MCLK_MULTIPLE_384;
            }
            return ESPIO_AUDIO_OK;
        case ESPIO_AUDIO_MCLK_POLICY_REQUIRED_256FS:
            clk_cfg->mclk_multiple = I2S_MCLK_MULTIPLE_256;
            return ESPIO_AUDIO_OK;
        case ESPIO_AUDIO_MCLK_POLICY_REQUIRED_384FS:
            clk_cfg->mclk_multiple = I2S_MCLK_MULTIPLE_384;
            return ESPIO_AUDIO_OK;
        default:
            return ESPIO_AUDIO_ERR_I2S_CONFIG;
    }
}

/**
 * @brief Build one standard-mode configuration shared across TX and RX.
 */
static int32_t audio_i2s_hal_build_std_config(const espio_audio_format_t* format,
                                              const espio_audio_i2s_pins_t* pins,
                                              espio_audio_mclk_policy_t mclk_policy,
                                              i2s_std_slot_mask_t rx_slot_mask,
                                              i2s_std_config_t* out_std_cfg) {
    if (!format || !pins || !out_std_cfg) {
        return ESPIO_INVALID_ARG;
    }

    if (format->serial_mode != ESPIO_AUDIO_SERIAL_MODE_I2S_PHILIPS) {
        ESPIO_LOGE(TAG, "unsupported serial mode");
        return ESPIO_AUDIO_ERR_INVALID_FORMAT;
    }

    if (pins->bclk_pin < 0 || pins->ws_pin < 0) {
        ESPIO_LOGE(TAG, "invalid I2S pin assignment");
        return ESPIO_INVALID_ARG;
    }

    i2s_data_bit_width_t data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    if (audio_i2s_hal_map_bit_width(format->bit_width, &data_bit_width) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "unsupported bit width");
        return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }

    i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_STEREO;
    if (audio_i2s_hal_map_slot_mode(format->channel_mode, &slot_mode) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "unsupported channel mode");
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(format->sample_rate_hz);
    if (audio_i2s_hal_apply_mclk_policy(mclk_policy, format->bit_width, &clk_cfg) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "unsupported MCLK policy");
        return ESPIO_AUDIO_ERR_I2S_CONFIG;
    }

    if (mclk_policy != ESPIO_AUDIO_MCLK_POLICY_DISABLED && pins->mclk_pin < 0) {
        ESPIO_LOGE(TAG, "selected MCLK policy requires an MCLK pin");
        return ESPIO_AUDIO_ERR_I2S_CONFIG;
    }

    gpio_num_t mclk_pin = I2S_GPIO_UNUSED;
    if (mclk_policy != ESPIO_AUDIO_MCLK_POLICY_DISABLED && pins->mclk_pin >= 0) {
        mclk_pin = (gpio_num_t)pins->mclk_pin;
    }

    *out_std_cfg = (i2s_std_config_t) {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(data_bit_width, slot_mode),
        .gpio_cfg = {
            .mclk = mclk_pin,
            .bclk = (gpio_num_t)pins->bclk_pin,
            .ws = (gpio_num_t)pins->ws_pin,
            .dout = pins->dout_pin >= 0 ? (gpio_num_t)pins->dout_pin : I2S_GPIO_UNUSED,
            .din = pins->din_pin >= 0 ? (gpio_num_t)pins->din_pin : I2S_GPIO_UNUSED,
        },
    };

    if (format->channel_mode == ESPIO_AUDIO_CHANNEL_MODE_MONO) {
        out_std_cfg->slot_cfg.slot_mask = rx_slot_mask;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Build one TDM-mode configuration for microphone capture.
 */
static int32_t audio_i2s_hal_build_tdm_config(const espio_audio_format_t* format,
                                              const espio_audio_i2s_pins_t* pins,
                                              espio_audio_mclk_policy_t mclk_policy,
                                              uint8_t channel_count,
                                              i2s_tdm_config_t* out_tdm_cfg) {
    if (!format || !pins || !out_tdm_cfg || channel_count == 0U || channel_count > 16U) {
        return ESPIO_INVALID_ARG;
    }

    if (format->serial_mode != ESPIO_AUDIO_SERIAL_MODE_I2S_PHILIPS) {
        ESPIO_LOGE(TAG, "unsupported TDM serial mode");
        return ESPIO_AUDIO_ERR_INVALID_FORMAT;
    }

    if (pins->bclk_pin < 0 || pins->ws_pin < 0 || pins->din_pin < 0) {
        ESPIO_LOGE(TAG, "invalid TDM pin assignment");
        return ESPIO_INVALID_ARG;
    }

    i2s_data_bit_width_t data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    if (audio_i2s_hal_map_bit_width(format->bit_width, &data_bit_width) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "unsupported TDM bit width");
        return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }

    i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_MONO;
    if (audio_i2s_hal_map_slot_mode(channel_count > 1U ? ESPIO_AUDIO_CHANNEL_MODE_STEREO : ESPIO_AUDIO_CHANNEL_MODE_MONO, &slot_mode) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "unsupported TDM slot mode");
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    i2s_tdm_clk_config_t clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(format->sample_rate_hz);
    switch (mclk_policy) {
        case ESPIO_AUDIO_MCLK_POLICY_DISABLED:
            break;
        case ESPIO_AUDIO_MCLK_POLICY_AUTO:
            if (format->bit_width == ESPIO_AUDIO_BIT_WIDTH_24) {
                clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
            }
            break;
        case ESPIO_AUDIO_MCLK_POLICY_REQUIRED_256FS:
            clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
            break;
        case ESPIO_AUDIO_MCLK_POLICY_REQUIRED_384FS:
            clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
            break;
        default:
            ESPIO_LOGE(TAG, "unsupported TDM MCLK policy");
            return ESPIO_AUDIO_ERR_I2S_CONFIG;
    }

    if (mclk_policy != ESPIO_AUDIO_MCLK_POLICY_DISABLED && pins->mclk_pin < 0) {
        ESPIO_LOGE(TAG, "selected TDM MCLK policy requires an MCLK pin");
        return ESPIO_AUDIO_ERR_I2S_CONFIG;
    }

    uint16_t slot_mask = 0U;
    for (uint8_t slot = 0; slot < channel_count; slot++) {
        slot_mask |= (uint16_t)(1U << slot);
    }

    i2s_tdm_slot_config_t slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(data_bit_width, slot_mode, slot_mask);
    slot_cfg.skip_mask = true;
    slot_cfg.total_slot = channel_count;

    *out_tdm_cfg = (i2s_tdm_config_t) {
        .clk_cfg = clk_cfg,
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .mclk = mclk_policy != ESPIO_AUDIO_MCLK_POLICY_DISABLED && pins->mclk_pin >= 0 ? (gpio_num_t)pins->mclk_pin : I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)pins->bclk_pin,
            .ws = (gpio_num_t)pins->ws_pin,
            .dout = pins->dout_pin >= 0 ? (gpio_num_t)pins->dout_pin : I2S_GPIO_UNUSED,
            .din = (gpio_num_t)pins->din_pin,
        },
    };

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Build one PDM-mode configuration for microphone capture.
 */
static int32_t audio_i2s_hal_build_pdm_config(const espio_audio_format_t* format,
                                              int clk_pin,
                                              int data_pin,
                                              bool hardware_high_pass_enabled,
                                              float hardware_high_pass_cutoff_hz,
                                              uint8_t hardware_amplify,
                                              i2s_pdm_rx_config_t* out_pdm_cfg) {
    if (!format || !out_pdm_cfg || clk_pin < 0 || data_pin < 0) {
        return ESPIO_INVALID_ARG;
    }

    if (format->bit_width != ESPIO_AUDIO_BIT_WIDTH_16) {
        ESPIO_LOGE(TAG, "PDM RX only supports 16-bit PCM");
        return ESPIO_AUDIO_ERR_BIT_WIDTH;
    }

    i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_STEREO;
    if (audio_i2s_hal_map_slot_mode(format->channel_mode, &slot_mode) != ESPIO_AUDIO_OK) {
        ESPIO_LOGE(TAG, "unsupported PDM channel mode");
        return ESPIO_AUDIO_ERR_CHANNEL_MODE;
    }

    i2s_pdm_rx_slot_config_t slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, slot_mode);
#if SOC_I2S_SUPPORTS_PDM_RX_HP_FILTER
    slot_cfg.hp_en = hardware_high_pass_enabled;
    if (hardware_high_pass_enabled && hardware_high_pass_cutoff_hz > 0.0f) {
        slot_cfg.hp_cut_off_freq_hz = hardware_high_pass_cutoff_hz;
    }
    if (hardware_amplify >= 1U && hardware_amplify <= 15U) {
        slot_cfg.amplify_num = hardware_amplify;
    }
#else
    (void)hardware_high_pass_enabled;
    (void)hardware_high_pass_cutoff_hz;
    (void)hardware_amplify;
#endif

    *out_pdm_cfg = (i2s_pdm_rx_config_t) {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(format->sample_rate_hz),
        .slot_cfg = slot_cfg,
        .gpio_cfg = {
            .clk = (gpio_num_t)clk_pin,
            .din = (gpio_num_t)data_pin,
        },
    };

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize one or both standard-mode channels.
 */
static int32_t audio_i2s_hal_init_channels(audio_i2s_hal_t* hal,
                                           const espio_audio_format_t* format,
                                           const espio_audio_i2s_pins_t* pins,
                                           int controller_id,
                                           espio_audio_mclk_policy_t mclk_policy,
                                           i2s_std_slot_mask_t rx_slot_mask,
                                           bool need_tx,
                                           bool need_rx) {
    if (!hal || !format || !pins || (!need_tx && !need_rx)) {
        return ESPIO_INVALID_ARG;
    }

    if ((need_tx && pins->dout_pin < 0) || (need_rx && pins->din_pin < 0)) {
        ESPIO_LOGE(TAG, "invalid I2S pin assignment");
        return ESPIO_INVALID_ARG;
    }

    memset(hal, 0, sizeof(*hal));

    i2s_std_config_t std_cfg = { 0 };
    int32_t build_result = audio_i2s_hal_build_std_config(format, pins, mclk_policy, rx_slot_mask, &std_cfg);
    if (build_result != ESPIO_AUDIO_OK) {
        return build_result;
    }

    i2s_port_t port = controller_id < 0 ? I2S_NUM_AUTO : (i2s_port_t)controller_id;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);

    esp_err_t err = i2s_new_channel(&chan_cfg, need_tx ? &hal->tx_handle : NULL, need_rx ? &hal->rx_handle : NULL);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to allocate I2S channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    if (hal->tx_handle) {
        err = i2s_channel_init_std_mode(hal->tx_handle, &std_cfg);
        if (err != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to initialize I2S TX channel: %s", esp_err_to_name(err));
            audio_i2s_hal_deinit(hal);
            return ESPIO_AUDIO_ERR_I2S_INIT;
        }
    }

    if (hal->rx_handle) {
        err = i2s_channel_init_std_mode(hal->rx_handle, &std_cfg);
        if (err != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to initialize I2S RX channel: %s", esp_err_to_name(err));
            audio_i2s_hal_deinit(hal);
            return ESPIO_AUDIO_ERR_I2S_INIT;
        }
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize the ESP-IDF I2S standard-mode TX path.
 */
int32_t audio_i2s_hal_init_tx(audio_i2s_hal_t* hal, const espio_audio_format_t* format, const espio_audio_i2s_pins_t* pins, int controller_id, espio_audio_mclk_policy_t mclk_policy) {
    return audio_i2s_hal_init_channels(hal, format, pins, controller_id, mclk_policy, I2S_STD_SLOT_LEFT, true, false);
}

/**
 * @brief Initialize the ESP-IDF I2S standard-mode RX path.
 */
int32_t audio_i2s_hal_init_rx(audio_i2s_hal_t* hal, const espio_audio_format_t* format, const espio_audio_i2s_pins_t* pins, int controller_id, espio_audio_mclk_policy_t mclk_policy) {
    return audio_i2s_hal_init_rx_with_slot_mask(hal, format, pins, controller_id, mclk_policy, I2S_STD_SLOT_LEFT);
}

/**
 * @brief Initialize the ESP-IDF I2S standard-mode RX path with one slot selection.
 */
int32_t audio_i2s_hal_init_rx_with_slot_mask(audio_i2s_hal_t* hal,
                                             const espio_audio_format_t* format,
                                             const espio_audio_i2s_pins_t* pins,
                                             int controller_id,
                                             espio_audio_mclk_policy_t mclk_policy,
                                             i2s_std_slot_mask_t slot_mask) {
    return audio_i2s_hal_init_channels(hal, format, pins, controller_id, mclk_policy, slot_mask, false, true);
}

/**
 * @brief Initialize paired TX and RX channels on one controller.
 */
int32_t audio_i2s_hal_init_full_duplex(audio_i2s_hal_t* hal, const espio_audio_format_t* format, const espio_audio_i2s_pins_t* pins, int controller_id, espio_audio_mclk_policy_t mclk_policy) {
    return audio_i2s_hal_init_channels(hal, format, pins, controller_id, mclk_policy, I2S_STD_SLOT_LEFT, true, true);
}

/**
 * @brief Initialize the ESP-IDF I2S PDM RX path.
 */
int32_t audio_i2s_hal_init_pdm_rx(audio_i2s_hal_t* hal,
                                  const espio_audio_format_t* format,
                                  int clk_pin,
                                  int data_pin,
                                  int controller_id,
                                  bool hardware_high_pass_enabled,
                                  float hardware_high_pass_cutoff_hz,
                                  uint8_t hardware_amplify) {
    if (!hal || !format) {
        return ESPIO_INVALID_ARG;
    }

    memset(hal, 0, sizeof(*hal));

    i2s_pdm_rx_config_t pdm_cfg = { 0 };
    int32_t build_result = audio_i2s_hal_build_pdm_config(format,
                                                          clk_pin,
                                                          data_pin,
                                                          hardware_high_pass_enabled,
                                                          hardware_high_pass_cutoff_hz,
                                                          hardware_amplify,
                                                          &pdm_cfg);
    if (build_result != ESPIO_AUDIO_OK) {
        return build_result;
    }

    i2s_port_t port = controller_id < 0 ? I2S_NUM_AUTO : (i2s_port_t)controller_id;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &hal->rx_handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to allocate PDM RX channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    err = i2s_channel_init_pdm_rx_mode(hal->rx_handle, &pdm_cfg);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to initialize PDM RX channel: %s", esp_err_to_name(err));
        audio_i2s_hal_deinit(hal);
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Initialize the ESP-IDF I2S TDM RX path.
 */
int32_t audio_i2s_hal_init_tdm_rx(audio_i2s_hal_t* hal,
                                  const espio_audio_format_t* format,
                                  const espio_audio_i2s_pins_t* pins,
                                  int controller_id,
                                  espio_audio_mclk_policy_t mclk_policy,
                                  uint8_t channel_count) {
    if (!hal || !format || !pins) {
        return ESPIO_INVALID_ARG;
    }

    memset(hal, 0, sizeof(*hal));

    i2s_tdm_config_t tdm_cfg = { 0 };
    int32_t build_result = audio_i2s_hal_build_tdm_config(format, pins, mclk_policy, channel_count, &tdm_cfg);
    if (build_result != ESPIO_AUDIO_OK) {
        return build_result;
    }

    i2s_port_t port = controller_id < 0 ? I2S_NUM_AUTO : (i2s_port_t)controller_id;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &hal->rx_handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to allocate TDM RX channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    err = i2s_channel_init_tdm_mode(hal->rx_handle, &tdm_cfg);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to initialize TDM RX channel: %s", esp_err_to_name(err));
        audio_i2s_hal_deinit(hal);
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Deinitialize the I2S transport state.
 */
void audio_i2s_hal_deinit(audio_i2s_hal_t* hal) {
    if (!hal) {
        return;
    }

    if (hal->tx_enabled) {
        (void)audio_i2s_hal_stop_tx(hal);
    }
    if (hal->rx_enabled) {
        (void)audio_i2s_hal_stop_rx(hal);
    }

    if (hal->tx_handle) {
        (void)i2s_del_channel(hal->tx_handle);
        hal->tx_handle = NULL;
    }
    if (hal->rx_handle) {
        (void)i2s_del_channel(hal->rx_handle);
        hal->rx_handle = NULL;
    }
}

/**
 * @brief Enable PCM transmission.
 */
int32_t audio_i2s_hal_start_tx(audio_i2s_hal_t* hal) {
    if (!hal || !hal->tx_handle) {
        return ESPIO_INVALID_ARG;
    }
    if (hal->tx_enabled) {
        return ESPIO_AUDIO_OK;
    }

    esp_err_t err = i2s_channel_enable(hal->tx_handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to enable I2S TX channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    hal->tx_enabled = true;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Disable PCM transmission.
 */
int32_t audio_i2s_hal_stop_tx(audio_i2s_hal_t* hal) {
    if (!hal || !hal->tx_handle) {
        return ESPIO_INVALID_ARG;
    }
    if (!hal->tx_enabled) {
        return ESPIO_AUDIO_OK;
    }

    esp_err_t err = i2s_channel_disable(hal->tx_handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to disable I2S TX channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    hal->tx_enabled = false;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Enable PCM reception.
 */
int32_t audio_i2s_hal_start_rx(audio_i2s_hal_t* hal) {
    if (!hal || !hal->rx_handle) {
        return ESPIO_INVALID_ARG;
    }
    if (hal->rx_enabled) {
        return ESPIO_AUDIO_OK;
    }

    esp_err_t err = i2s_channel_enable(hal->rx_handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to enable I2S RX channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    hal->rx_enabled = true;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Disable PCM reception.
 */
int32_t audio_i2s_hal_stop_rx(audio_i2s_hal_t* hal) {
    if (!hal || !hal->rx_handle) {
        return ESPIO_INVALID_ARG;
    }
    if (!hal->rx_enabled) {
        return ESPIO_AUDIO_OK;
    }

    esp_err_t err = i2s_channel_disable(hal->rx_handle);
    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to disable I2S RX channel: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_INIT;
    }

    hal->rx_enabled = false;
    return ESPIO_AUDIO_OK;
}

/**
 * @brief Write PCM data to the active TX channel.
 */
int32_t audio_i2s_hal_write(audio_i2s_hal_t* hal, const void* data, size_t size, size_t* bytes_written, uint32_t timeout_ms) {
    if (!hal || !hal->tx_handle || !data || size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    size_t local_written = 0;
    esp_err_t err = i2s_channel_write(hal->tx_handle, data, size, &local_written, pdMS_TO_TICKS(timeout_ms));
    if (bytes_written) {
        *bytes_written = local_written;
    }

    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to write PCM data: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_WRITE;
    }

    return ESPIO_AUDIO_OK;
}

/**
 * @brief Read PCM data from the active RX channel.
 */
int32_t audio_i2s_hal_read(audio_i2s_hal_t* hal, void* data, size_t size, size_t* bytes_read, uint32_t timeout_ms) {
    if (!hal || !hal->rx_handle || !data || size == 0U) {
        return ESPIO_INVALID_ARG;
    }

    size_t local_read = 0;
    esp_err_t err = i2s_channel_read(hal->rx_handle, data, size, &local_read, pdMS_TO_TICKS(timeout_ms));
    if (bytes_read) {
        *bytes_read = local_read;
    }

    if (err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to read PCM data: %s", esp_err_to_name(err));
        return ESPIO_AUDIO_ERR_I2S_WRITE;
    }

    return ESPIO_AUDIO_OK;
}
