/**
 * @file amp_max98357a.h
 * @brief MAX98357A amplifier driver registration and configuration.
 */

#ifndef ESPIO_AMP_MAX98357A_H
#define ESPIO_AMP_MAX98357A_H

#include "espio/audio_amp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MAX98357A driver configuration.
 */
typedef struct {
    int sd_mode_pin;                 ///< Shutdown / mode GPIO, or AUDIO_PIN_UNUSED when tied off-board.
    bool sd_mode_active_high;        ///< True when a high level enables output.
} espio_audio_max98357a_config_t;

/**
 * @brief Get the MAX98357A driver instance.
 * @return Driver descriptor for `audio_create()`.
 */
const espio_audio_driver_t* espio_audio_driver_max98357a(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AMP_MAX98357A_H */
