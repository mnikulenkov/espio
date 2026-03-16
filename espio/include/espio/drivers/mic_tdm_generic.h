/**
 * @file mic_tdm_generic.h
 * @brief Generic TDM microphone driver registration and configuration.
 */

#ifndef ESPIO_MIC_TDM_GENERIC_H
#define ESPIO_MIC_TDM_GENERIC_H

#include "espio/audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generic TDM microphone driver configuration.
 */
typedef struct {
    espio_audio_mic_tdm_config_t tdm_config; ///< TDM transport configuration.
} espio_audio_mic_tdm_generic_config_t;

/**
 * @brief Get the generic TDM microphone driver instance.
 * @return Driver descriptor for `espio_audio_mic_create()`.
 */
const espio_audio_mic_driver_t* espio_audio_mic_driver_tdm_generic(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_MIC_TDM_GENERIC_H */
