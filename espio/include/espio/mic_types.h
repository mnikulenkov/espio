/**
 * @file mic_types.h
 * @brief Shared public microphone types.
 */

#ifndef ESPIO_MIC_TYPES_H
#define ESPIO_MIC_TYPES_H

#include "espio/audio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Supported microphone transport families.
 */
typedef enum {
    ESPIO_AUDIO_MIC_INTERFACE_I2S = 0,   ///< Standard I2S digital microphone transport.
    ESPIO_AUDIO_MIC_INTERFACE_PDM = 1,   ///< Dedicated PDM microphone transport.
    ESPIO_AUDIO_MIC_INTERFACE_ADC = 2,   ///< Analog microphone transport through ADC.
    ESPIO_AUDIO_MIC_INTERFACE_USB = 3,   ///< USB audio transport for future host-mode support.
    ESPIO_AUDIO_MIC_INTERFACE_TDM = 4,   ///< Multi-channel TDM transport.
} espio_audio_mic_interface_type_t;

/**
 * @brief Supported microphone gain-control strategies.
 */
typedef enum {
    ESPIO_AUDIO_MIC_GAIN_TYPE_NONE = 0,      ///< No gain control is available.
    ESPIO_AUDIO_MIC_GAIN_TYPE_HARDWARE = 1,  ///< Gain is controlled by the microphone hardware.
    ESPIO_AUDIO_MIC_GAIN_TYPE_SOFTWARE = 2,  ///< Gain is applied in the RX processing path.
} espio_audio_mic_gain_type_t;

/**
 * @brief Runtime capabilities exposed by a microphone instance.
 */
typedef struct {
    espio_audio_mic_interface_type_t interface_type; ///< Concrete transport used by this microphone.
    espio_audio_mic_gain_type_t gain_type;  ///< Gain strategy reported by the driver.
    int8_t min_gain_db;                     ///< Lowest supported gain in dB.
    int8_t max_gain_db;                     ///< Highest supported gain in dB.
    int8_t default_gain_db;                 ///< Default gain in dB.
    uint32_t min_sample_rate_hz;            ///< Lowest supported sample rate.
    uint32_t max_sample_rate_hz;            ///< Highest supported sample rate.
    const espio_audio_bit_width_t* supported_bit_widths; ///< Supported PCM widths.
    size_t supported_bit_width_count;       ///< Number of supported PCM widths.
    bool supports_stereo;                   ///< True when stereo capture is supported.
    bool supports_high_pass_filter;         ///< True when the driver exposes native HPF.
    bool supports_agc;                      ///< True when the driver exposes native AGC.
    bool supports_sleep_mode;               ///< True when the driver can sleep or power down.
} espio_audio_mic_caps_t;

/**
 * @brief Standard I2S microphone transport configuration.
 */
typedef struct {
    espio_audio_format_t format;       ///< Requested PCM format.
    espio_audio_i2s_pins_t pins;       ///< Shared I2S pin assignment. Uses `din_pin`.
    int controller_id;                 ///< I2S controller index, or a negative value for auto-selection.
    espio_audio_mclk_policy_t mclk_policy; ///< Requested MCLK policy.
    bool capture_right_slot;           ///< True to capture the right slot in mono mode. Stereo capture reads both slots.
} espio_audio_mic_i2s_config_t;

/**
 * @brief PDM microphone transport configuration.
 */
typedef struct {
    espio_audio_format_t format;       ///< Requested PCM output format.
    int clk_pin;                       ///< PDM clock pin.
    int data_pin;                      ///< PDM data input pin.
    int controller_id;                 ///< I2S controller index, or a negative value for auto-selection.
    bool hardware_high_pass_enabled;   ///< True to enable the hardware PDM high-pass filter.
    float hardware_high_pass_cutoff_hz;///< Requested hardware HPF cutoff in Hertz.
    uint8_t hardware_amplify;          ///< Hardware PCM amplification factor in the 1-15 range.
} espio_audio_mic_pdm_config_t;

/**
 * @brief ADC microphone transport configuration.
 */
typedef struct {
    espio_audio_format_t format;       ///< Requested PCM output format. ADC capture is mono.
    uint32_t sample_rate_hz;           ///< Requested sample rate.
    int adc_unit;                      ///< ADC unit index.
    int adc_channel;                   ///< ADC channel index.
    uint8_t attenuation_db;            ///< ADC attenuation selector. Valid values are 0, 2, 6, and 12.
    uint16_t frame_size;               ///< DMA frame size in bytes.
    uint16_t max_store_buffer_size;    ///< ADC driver pool size in bytes.
    int8_t gain_db;                    ///< External preamp gain, when present.
} espio_audio_mic_adc_config_t;

/**
 * @brief USB microphone transport configuration reserved for future use.
 */
typedef struct {
    uint32_t sample_rate_hz;           ///< Requested sample rate.
    uint8_t channel_count;             ///< Requested channel count.
    espio_audio_bit_width_t bit_width; ///< Requested PCM width.
    uint32_t connection_timeout_ms;    ///< Maximum device-attach wait time.
} espio_audio_mic_usb_config_t;

/**
 * @brief TDM microphone transport configuration.
 */
typedef struct {
    espio_audio_format_t format;       ///< Requested PCM format.
    espio_audio_i2s_pins_t pins;       ///< Shared TDM pin assignment. Uses `din_pin`.
    int controller_id;                 ///< I2S controller index, or a negative value for auto-selection.
    espio_audio_mclk_policy_t mclk_policy; ///< Requested MCLK policy.
    uint8_t channel_count;             ///< Number of TDM channels to capture.
    bool hardware_high_pass_enabled;   ///< Reserved for future TDM hardware filtering support.
} espio_audio_mic_tdm_config_t;

/**
 * @brief Normalized transport descriptor extracted from a concrete mic config.
 */
typedef struct {
    espio_audio_mic_interface_type_t interface_type; ///< Concrete transport family.
    union {
        espio_audio_mic_i2s_config_t i2s; ///< I2S transport configuration.
        espio_audio_mic_pdm_config_t pdm; ///< PDM transport configuration.
        espio_audio_mic_adc_config_t adc; ///< ADC transport configuration.
        espio_audio_mic_usb_config_t usb; ///< USB transport configuration.
        espio_audio_mic_tdm_config_t tdm; ///< TDM transport configuration.
    } config;
} espio_audio_mic_transport_config_t;

/**
 * @brief Optional hardware filter configuration reported to drivers.
 */
typedef struct {
    bool high_pass_enabled;          ///< True when hardware HPF should be enabled.
    float high_pass_cutoff_hz;       ///< Requested HPF cutoff in Hertz.
} espio_audio_mic_filter_config_t;

/**
 * @brief Driver contract implemented by concrete microphone drivers.
 */
typedef struct {
    const char* name;   ///< Driver name for logs and diagnostics.
    espio_audio_mic_interface_type_t interface_type; ///< Primary transport family used by the driver.

    /**
     * @brief Query microphone capabilities for a specific driver configuration.
     * @param config Driver-specific configuration.
     * @param caps Output capability structure.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*query_caps)(const void* config, espio_audio_mic_caps_t* caps);

    /**
     * @brief Normalize the transport configuration used by the generic RX layer.
     * @param config Driver-specific configuration.
     * @param transport_config Output normalized transport configuration.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*get_transport_config)(const void* config, espio_audio_mic_transport_config_t* transport_config);

    /**
     * @brief Initialize driver-specific state.
     * @param config Driver-specific configuration.
     * @return Driver context on success, NULL on failure.
     */
    void* (*init)(const void* config);

    /**
     * @brief Deinitialize driver-specific state.
     * @param ctx Driver context.
     */
    void (*deinit)(void* ctx);

    /**
     * @brief Set microphone power state, when the driver supports it.
     * @param ctx Driver context.
     * @param on true to enable capture, false to disable it.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*set_power)(void* ctx, bool on);

    /**
     * @brief Set hardware gain, when the driver supports it.
     * @param ctx Driver context.
     * @param gain_db Requested gain in dB.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*set_gain)(void* ctx, int8_t gain_db);

    /**
     * @brief Read the current hardware gain, when the driver supports it.
     * @param ctx Driver context.
     * @param gain_db Output gain in dB.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*get_gain)(void* ctx, int8_t* gain_db);

    /**
     * @brief Configure hardware filter features, when the driver supports them.
     * @param ctx Driver context.
     * @param config Requested filter configuration.
     * @return AUDIO_OK on success, or error code on failure.
     */
    int32_t (*configure_filters)(void* ctx, const espio_audio_mic_filter_config_t* config);
} espio_audio_mic_driver_t;

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_MIC_TYPES_H */
