/**
 * @file display_ili9341.h
 * @brief ILI9341 Display Driver Configuration and Registration
 * 
 * This header provides the ILI9341-specific configuration structure
 * and driver registration. Include this file when using ILI9341 displays.
 */

#ifndef ESPIO_DISPLAY_ILI9341_H
#define ESPIO_DISPLAY_ILI9341_H

#include "espio/display.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SPI clock speed in Hz
 */
#define ESPIO_DISPLAY_ILI9341_SPI_CLOCK_DEFAULT   10000000    ///< 10 MHz default
#define ESPIO_DISPLAY_ILI9341_SPI_CLOCK_FAST      40000000    ///< 40 MHz fast
#define ESPIO_DISPLAY_ILI9341_SPI_CLOCK_SLOW      5000000     ///< 5 MHz slow

/**
 * @brief SPI mode for ILI9341
 */
#define ESPIO_DISPLAY_ILI9341_SPI_MODE_DEFAULT    3           ///< SPI Mode 3 (CPOL=1, CPHA=1)

/**
 * @brief SPI transfer size constants
 */
#define ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_AUTO    0       ///< Auto-calculate transfer size
#define ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_MIN     4096    ///< Minimum transfer size (4KB)
#define ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_DEFAULT 32768   ///< Default maximum transfer size (32KB)

/**
 * @brief ILI9341 SPI hardware configuration
 */
typedef struct {
    int pin_sck;           ///< SPI Clock pin
    int pin_mosi;          ///< SPI MOSI (Master Out Slave In) pin
    int pin_miso;          ///< SPI MISO (Master In Slave Out) pin, -1 if not used
    int pin_cs;            ///< SPI Chip Select pin
    int pin_dc;            ///< Data/Command pin
    int pin_rst;           ///< Reset pin, -1 if not used
    int spi_host_id;       ///< SPI host ID (2 for SPI2_HOST, 3 for SPI3_HOST; SPI1 is reserved for flash)
    uint32_t spi_clock_hz; ///< SPI clock frequency in Hz
    int spi_mode;          ///< SPI mode (0-3)
    uint32_t max_transfer_size;  ///< Max SPI transfer size in bytes (0 = auto-calculate)
} espio_display_ili9341_spi_config_t;

/**
 * @brief ILI9341 backlight configuration
 */
typedef struct {
    int pin;               ///< Backlight PWM pin, -1 if not used
    uint32_t pwm_freq_hz;  ///< PWM frequency in Hz (e.g., 1000)
    bool active_low;       ///< true if backlight is active low
} espio_display_ili9341_backlight_config_t;

/**
 * @brief ILI9341 display orientation configuration
 */
typedef struct {
    bool swap_xy;          ///< Swap X and Y axes (landscape vs portrait)
    bool mirror_x;         ///< Mirror X axis
    bool mirror_y;         ///< Mirror Y axis
    bool rgb_order;        ///< RGB order: true=RGB, false=BGR
} espio_display_ili9341_orientation_config_t;

/**
 * @brief ILI9341 complete hardware configuration
 */
typedef struct {
    espio_display_ili9341_spi_config_t spi;       ///< SPI configuration
    espio_display_ili9341_backlight_config_t backlight; ///< Backlight configuration
    espio_display_ili9341_orientation_config_t orientation; ///< Orientation configuration
    uint16_t width;                             ///< Display width in pixels (typically 240 or 320)
    uint16_t height;                            ///< Display height in pixels (typically 320 or 240)
    uint8_t initial_brightness;                 ///< Initial backlight brightness (0-255)
} espio_display_ili9341_config_t;

/**
 * @brief Get the ILI9341 driver instance
 * 
 * Use this to register the ILI9341 driver with the display abstraction.
 * 
 * @return Pointer to the ILI9341 driver structure
 */
const espio_display_driver_t* espio_display_driver_ili9341(void);

/**
 * @brief Helper macro to create a basic ILI9341 configuration
 * 
 * @param sck SPI clock pin
 * @param mosi SPI MOSI pin
 * @param miso SPI MISO pin (-1 if not used)
 * @param cs Chip select pin
 * @param dc Data/Command pin
 * @param rst Reset pin (-1 if not used)
 * @param bl Backlight pin (-1 if not used)
 * @param host SPI host ID (2 or 3)
 * @param w Display width
 * @param h Display height
 */
#define ESPIO_DISPLAY_ILI9341_CONFIG_BASIC(sck, mosi, miso, cs, dc, rst, bl, host, w, h) \
    ((espio_display_ili9341_config_t){ \
        .spi = { \
            .pin_sck = (sck), \
            .pin_mosi = (mosi), \
            .pin_miso = (miso), \
            .pin_cs = (cs), \
            .pin_dc = (dc), \
            .pin_rst = (rst), \
            .spi_host_id = (host), \
            .spi_clock_hz = ESPIO_DISPLAY_ILI9341_SPI_CLOCK_DEFAULT, \
            .spi_mode = ESPIO_DISPLAY_ILI9341_SPI_MODE_DEFAULT, \
            .max_transfer_size = ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_AUTO \
        }, \
        .backlight = { \
            .pin = (bl), \
            .pwm_freq_hz = 1000, \
            .active_low = false \
        }, \
        .orientation = { \
            .swap_xy = false, \
            .mirror_x = false, \
            .mirror_y = false, \
            .rgb_order = false \
        }, \
        .width = (w), \
        .height = (h), \
        .initial_brightness = 255 \
    })

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_DISPLAY_ILI9341_H */
