/**
 * @file audio_types.h
 * @brief Shared public audio types.
 */

#ifndef ESPIO_AUDIO_TYPES_H
#define ESPIO_AUDIO_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sentinel value for an unused pin in the public API.
 */
#define ESPIO_AUDIO_PIN_UNUSED (-1)

/**
 * @brief Amplifier audio input class.
 */
typedef enum {
    ESPIO_AUDIO_INPUT_CLASS_DIGITAL = 0,               ///< Digital serial audio input.
    ESPIO_AUDIO_INPUT_CLASS_ANALOG_WITH_CONTROL = 1,   ///< Analog audio input with a digital control surface.
    ESPIO_AUDIO_INPUT_CLASS_ANALOG = 2                 ///< Pure analog amplifier without a digital audio path.
} espio_audio_input_class_t;

/**
 * @brief Control-plane bus used by the amplifier.
 */
typedef enum {
    ESPIO_AUDIO_CONTROL_BUS_NONE = 0,  ///< No control bus is exposed.
    ESPIO_AUDIO_CONTROL_BUS_GPIO = 1,  ///< Control is driven through GPIO pins.
    ESPIO_AUDIO_CONTROL_BUS_I2C = 2    ///< Control is driven through I2C.
} espio_audio_control_bus_t;

/**
 * @brief Power-control mechanism used by the amplifier.
 */
typedef enum {
    ESPIO_AUDIO_POWER_CONTROL_NONE = 0,          ///< No explicit power control is available.
    ESPIO_AUDIO_POWER_CONTROL_GPIO = 1,          ///< Power or shutdown is controlled by GPIO.
    ESPIO_AUDIO_POWER_CONTROL_CONTROL_BUS = 2    ///< Power is controlled through a control bus.
} espio_audio_power_control_t;

/**
 * @brief MCLK policy required by the transport path.
 */
typedef enum {
    ESPIO_AUDIO_MCLK_POLICY_DISABLED = 0,       ///< MCLK must stay disabled.
    ESPIO_AUDIO_MCLK_POLICY_AUTO = 1,           ///< MCLK multiple is derived from the selected format.
    ESPIO_AUDIO_MCLK_POLICY_REQUIRED_256FS = 2, ///< MCLK must be provided at 256 * sample rate.
    ESPIO_AUDIO_MCLK_POLICY_REQUIRED_384FS = 3  ///< MCLK must be provided at 384 * sample rate.
} espio_audio_mclk_policy_t;

/**
 * @brief Supported serial audio framing.
 */
typedef enum {
    ESPIO_AUDIO_SERIAL_MODE_I2S_PHILIPS = 0    ///< Standard Philips I2S framing.
} espio_audio_serial_mode_t;

/**
 * @brief Supported PCM bit widths.
 */
typedef enum {
    ESPIO_AUDIO_BIT_WIDTH_16 = 16,   ///< Signed 16-bit PCM.
    ESPIO_AUDIO_BIT_WIDTH_24 = 24,   ///< Signed 24-bit PCM.
    ESPIO_AUDIO_BIT_WIDTH_32 = 32    ///< Signed 32-bit PCM.
} espio_audio_bit_width_t;

/**
 * @brief Audio channel layout.
 */
typedef enum {
    ESPIO_AUDIO_CHANNEL_MODE_MONO = 1,    ///< Single-channel playback.
    ESPIO_AUDIO_CHANNEL_MODE_STEREO = 2   ///< Two-channel playback.
} espio_audio_channel_mode_t;

/**
 * @brief PCM playback format.
 */
typedef struct {
    uint32_t sample_rate_hz;              ///< Requested sample rate.
    espio_audio_bit_width_t bit_width;    ///< PCM sample width.
    espio_audio_channel_mode_t channel_mode; ///< Mono or stereo playback.
    espio_audio_serial_mode_t serial_mode; ///< Digital framing mode.
} espio_audio_format_t;

/**
 * @brief I2S signal pin mapping.
 */
typedef struct {
    int bclk_pin;     ///< Bit-clock GPIO.
    int ws_pin;       ///< Word-select / LRCLK GPIO.
    int dout_pin;     ///< Serial data output GPIO, or ESPIO_AUDIO_PIN_UNUSED when RX-only.
    int din_pin;      ///< Serial data input GPIO, or ESPIO_AUDIO_PIN_UNUSED when TX-only.
    int mclk_pin;     ///< Optional master-clock GPIO or ESPIO_AUDIO_PIN_UNUSED.
} espio_audio_i2s_pins_t;

/**
 * @brief Runtime capabilities exposed by an amplifier instance.
 */
typedef struct {
    espio_audio_input_class_t input_class;             ///< Audio input class.
    espio_audio_control_bus_t control_bus;             ///< Control-plane bus type.
    espio_audio_power_control_t power_control;         ///< Power-control mechanism.
    espio_audio_mclk_policy_t mclk_policy;             ///< Required MCLK policy.
    bool supports_mute;                                ///< True when mute can be controlled explicitly.
    bool supports_hardware_volume;                     ///< True when the amplifier has native volume control.
    bool supports_software_volume;                     ///< True when the transport may apply software gain.
    bool supports_dsp;                                 ///< True when the amplifier exposes DSP features.
    uint32_t min_sample_rate_hz;                       ///< Lowest supported sample rate.
    uint32_t max_sample_rate_hz;                       ///< Highest supported sample rate.
    const espio_audio_bit_width_t* supported_bit_widths; ///< Supported PCM widths.
    size_t supported_bit_width_count;                  ///< Number of supported PCM widths.
    const espio_audio_serial_mode_t* supported_serial_modes; ///< Supported serial framing modes.
    size_t supported_serial_mode_count;                ///< Number of supported serial framing modes.
} espio_audio_caps_t;

/**
 * @brief Playback transport configuration for digital-input amplifiers.
 */
typedef struct {
    espio_audio_format_t format;           ///< Requested PCM format.
    espio_audio_i2s_pins_t pins;           ///< I2S pin assignment.
    int controller_id;                     ///< I2S controller index, or a negative value for auto-selection.
    uint32_t write_timeout_ms;             ///< Default blocking timeout for writes.
    espio_audio_mclk_policy_t mclk_policy; ///< Override for MCLK policy, or AUTO to use amplifier caps.
} espio_audio_tx_config_t;

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_AUDIO_TYPES_H */
