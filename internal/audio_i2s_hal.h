/**
 * @file audio_i2s_hal.h
 * @brief Private ESP-IDF I2S transport wrapper shared by amp and mic modules.
 */

#ifndef AUDIO_I2S_HAL_H
#define AUDIO_I2S_HAL_H

#include "espio/audio_err.h"
#include "espio/audio_types.h"
#include <stdbool.h>
#include "driver/i2s_std.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Private I2S transport state.
 */
typedef struct {
    i2s_chan_handle_t tx_handle; ///< ESP-IDF transmit channel handle, when configured.
    i2s_chan_handle_t rx_handle; ///< ESP-IDF receive channel handle, when configured.
    bool tx_enabled;             ///< True when the TX channel is enabled.
    bool rx_enabled;             ///< True when the RX channel is enabled.
} audio_i2s_hal_t;

int32_t audio_i2s_hal_init_tx(audio_i2s_hal_t* hal, const espio_audio_format_t* format, const espio_audio_i2s_pins_t* pins, int controller_id, espio_audio_mclk_policy_t mclk_policy);
int32_t audio_i2s_hal_init_rx(audio_i2s_hal_t* hal, const espio_audio_format_t* format, const espio_audio_i2s_pins_t* pins, int controller_id, espio_audio_mclk_policy_t mclk_policy);
int32_t audio_i2s_hal_init_rx_with_slot_mask(audio_i2s_hal_t* hal,
                                             const espio_audio_format_t* format,
                                             const espio_audio_i2s_pins_t* pins,
                                             int controller_id,
                                             espio_audio_mclk_policy_t mclk_policy,
                                             i2s_std_slot_mask_t slot_mask);
int32_t audio_i2s_hal_init_full_duplex(audio_i2s_hal_t* hal, const espio_audio_format_t* format, const espio_audio_i2s_pins_t* pins, int controller_id, espio_audio_mclk_policy_t mclk_policy);
int32_t audio_i2s_hal_init_pdm_rx(audio_i2s_hal_t* hal,
                                  const espio_audio_format_t* format,
                                  int clk_pin,
                                  int data_pin,
                                  int controller_id,
                                  bool hardware_high_pass_enabled,
                                  float hardware_high_pass_cutoff_hz,
                                  uint8_t hardware_amplify);
int32_t audio_i2s_hal_init_tdm_rx(audio_i2s_hal_t* hal,
                                  const espio_audio_format_t* format,
                                  const espio_audio_i2s_pins_t* pins,
                                  int controller_id,
                                  espio_audio_mclk_policy_t mclk_policy,
                                  uint8_t channel_count);
void audio_i2s_hal_deinit(audio_i2s_hal_t* hal);
int32_t audio_i2s_hal_start_tx(audio_i2s_hal_t* hal);
int32_t audio_i2s_hal_stop_tx(audio_i2s_hal_t* hal);
int32_t audio_i2s_hal_start_rx(audio_i2s_hal_t* hal);
int32_t audio_i2s_hal_stop_rx(audio_i2s_hal_t* hal);
int32_t audio_i2s_hal_write(audio_i2s_hal_t* hal, const void* data, size_t size, size_t* bytes_written, uint32_t timeout_ms);
int32_t audio_i2s_hal_read(audio_i2s_hal_t* hal, void* data, size_t size, size_t* bytes_read, uint32_t timeout_ms);

#endif /* AUDIO_I2S_HAL_H */
