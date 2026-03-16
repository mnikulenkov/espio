/**
 * @file mic_pdm_generic.h
 * @brief Generic PDM microphone driver registration and configuration.
 */

#ifndef ESPIO_MIC_PDM_GENERIC_H
#define ESPIO_MIC_PDM_GENERIC_H

#include "espio/audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generic PDM microphone driver configuration.
 */
typedef struct {
    espio_audio_mic_pdm_config_t pdm_config; ///< PDM transport configuration.
} espio_audio_mic_pdm_generic_config_t;

/**
 * @brief Get the generic PDM microphone driver instance.
 * @return Driver descriptor for `espio_audio_mic_create()`.
 */
const espio_audio_mic_driver_t* espio_audio_mic_driver_pdm_generic(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_MIC_PDM_GENERIC_H */
