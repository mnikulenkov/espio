/**
 * @file mic_adc_generic.h
 * @brief Generic ADC microphone driver registration and configuration.
 */

#ifndef ESPIO_MIC_ADC_GENERIC_H
#define ESPIO_MIC_ADC_GENERIC_H

#include "espio/audio_mic.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Generic ADC microphone driver configuration.
 */
typedef struct {
    espio_audio_mic_adc_config_t adc_config; ///< ADC transport configuration.
} espio_audio_mic_adc_generic_config_t;

/**
 * @brief Get the generic ADC microphone driver instance.
 * @return Driver descriptor for `espio_audio_mic_create()`.
 */
const espio_audio_mic_driver_t* espio_audio_mic_driver_adc_generic(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_MIC_ADC_GENERIC_H */
