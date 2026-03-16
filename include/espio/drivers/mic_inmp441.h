/**
 * @file mic_inmp441.h
 * @brief INMP441 microphone driver registration and configuration.
 */

#ifndef ESPIO_MIC_INMP441_H
#define ESPIO_MIC_INMP441_H

#include "espio/audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief INMP441 driver configuration.
 */
typedef struct {
    espio_audio_mic_i2s_config_t i2s_config; ///< Standard I2S transport configuration.
    int lr_pin;                       ///< Optional L/R select GPIO, or ESPIO_AUDIO_PIN_UNUSED when strapped externally.
    bool left_channel;                ///< True to sample on the left slot, false for the right slot.
    bool left_channel_level_high;     ///< True when a high level selects the left slot.
    int chipen_pin;                   ///< Optional CHIPEN GPIO, or ESPIO_AUDIO_PIN_UNUSED when tied high externally.
    bool chipen_level_high;           ///< True when a high level enables the microphone.
} espio_audio_mic_inmp441_config_t;

/**
 * @brief Get the INMP441 driver instance.
 * @return Driver descriptor for `espio_audio_mic_create()`.
 */
const espio_audio_mic_driver_t* espio_audio_mic_driver_inmp441(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_MIC_INMP441_H */
