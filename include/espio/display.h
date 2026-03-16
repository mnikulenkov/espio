/**
 * @file display.h
 * @brief Display abstraction API.
 */

#ifndef ESPIO_DISPLAY_H
#define ESPIO_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "espio/display_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque display handle.
 */
typedef struct espio_display espio_display_t;

/**
 * @brief Display pixel format.
 */
typedef enum {
    ESPIO_DISPLAY_PIXEL_FORMAT_RGB565 = 0,
    ESPIO_DISPLAY_PIXEL_FORMAT_RGB888,
    ESPIO_DISPLAY_PIXEL_FORMAT_MONO,
    ESPIO_DISPLAY_PIXEL_FORMAT_MONO_INVERT,
    ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_4,
    ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_8,
    ESPIO_DISPLAY_PIXEL_FORMAT_INDEXED_8,
} espio_display_pixel_format_t;

/**
 * @brief Return bytes per pixel for unpacked formats.
 */
static inline size_t espio_display_format_bytes_per_pixel(espio_display_pixel_format_t format) {
    switch (format) {
        case ESPIO_DISPLAY_PIXEL_FORMAT_RGB565:
            return 2U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_RGB888:
            return 3U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_8:
        case ESPIO_DISPLAY_PIXEL_FORMAT_INDEXED_8:
            return 1U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_MONO:
        case ESPIO_DISPLAY_PIXEL_FORMAT_MONO_INVERT:
        case ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_4:
        default:
            return 0U;
    }
}

/**
 * @brief Return bits per pixel for one pixel format.
 */
static inline size_t espio_display_format_bits_per_pixel(espio_display_pixel_format_t format) {
    switch (format) {
        case ESPIO_DISPLAY_PIXEL_FORMAT_RGB565:
            return 16U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_RGB888:
            return 24U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_MONO:
        case ESPIO_DISPLAY_PIXEL_FORMAT_MONO_INVERT:
            return 1U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_4:
            return 4U;
        case ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_8:
        case ESPIO_DISPLAY_PIXEL_FORMAT_INDEXED_8:
            return 8U;
        default:
            return 0U;
    }
}

/**
 * @brief Tell whether the format is stored packed inside bytes.
 */
static inline bool espio_display_format_is_packed(espio_display_pixel_format_t format) {
    return format == ESPIO_DISPLAY_PIXEL_FORMAT_MONO ||
           format == ESPIO_DISPLAY_PIXEL_FORMAT_MONO_INVERT ||
           format == ESPIO_DISPLAY_PIXEL_FORMAT_GRAYSCALE_4;
}

/**
 * @brief Display technology type.
 */
typedef enum {
    ESPIO_DISPLAY_TYPE_UNKNOWN = 0,
    ESPIO_DISPLAY_TYPE_LCD,
    ESPIO_DISPLAY_TYPE_OLED,
    ESPIO_DISPLAY_TYPE_EPAPER,
    ESPIO_DISPLAY_TYPE_LED_MATRIX,
    ESPIO_DISPLAY_TYPE_SEGMENT_LCD,
    ESPIO_DISPLAY_TYPE_VIRTUAL,
} espio_display_type_t;

/**
 * @brief Runtime display rotation.
 */
typedef enum {
    ESPIO_DISPLAY_ROTATION_0 = 0,
    ESPIO_DISPLAY_ROTATION_90 = 1,
    ESPIO_DISPLAY_ROTATION_180 = 2,
    ESPIO_DISPLAY_ROTATION_270 = 3,
} espio_display_rotation_t;

/**
 * @brief Extended display power state.
 */
typedef enum {
    ESPIO_DISPLAY_POWER_OFF = 0,
    ESPIO_DISPLAY_POWER_ON,
    ESPIO_DISPLAY_POWER_SLEEP,
    ESPIO_DISPLAY_POWER_DEEP_SLEEP,
    ESPIO_DISPLAY_POWER_STANDBY,
} espio_display_power_state_t;

/**
 * @brief Display capability snapshot.
 */
typedef struct {
    uint32_t supported_formats;
    bool has_backlight;
    bool has_partial_update;
    bool has_continuous_refresh;
    bool has_async_operations;
    bool has_runtime_rotation;
    bool has_vsync;
    bool has_read_pixel;
    uint32_t min_refresh_ms;
    uint32_t max_refresh_hz;
    uint32_t typical_refresh_ms;
    uint16_t physical_width_mm;
    uint16_t physical_height_mm;
    espio_display_type_t display_type;
} espio_display_caps_t;

/**
 * @brief Refresh mode supported by the driver.
 */
typedef enum {
    ESPIO_DISPLAY_REFRESH_ON_DEMAND = 0,
    ESPIO_DISPLAY_REFRESH_CONTINUOUS = 1,
} espio_display_refresh_mode_t;

/**
 * @brief Continuous refresh configuration.
 */
typedef struct {
    int refresh_rate_hz;
    bool vsync_enabled;
} espio_display_refresh_config_t;

/**
 * @brief Async flush callback.
 */
typedef void (*espio_display_async_callback_t)(void* user_ctx, int32_t result);

/**
 * @brief Async display state.
 */
typedef enum {
    ESPIO_DISPLAY_ASYNC_IDLE = 0,
    ESPIO_DISPLAY_ASYNC_FLUSHING,
    ESPIO_DISPLAY_ASYNC_PARTIAL,
    ESPIO_DISPLAY_ASYNC_CANCELLED,
} espio_display_async_state_t;

/**
 * @brief Concrete driver contract for one display backend.
 */
typedef struct espio_display_driver {
    const char* name;
    void* (*init)(const void* config, uint16_t* width, uint16_t* height);
    void (*deinit)(void* ctx);
    espio_display_pixel_format_t (*get_pixel_format)(void* ctx);
    int32_t (*draw_bitmap)(void* ctx, int x, int y, int width, int height, const void* data);
    int32_t (*fill_rect)(void* ctx, int x, int y, int width, int height, uint64_t color);
    int32_t (*flush)(void* ctx, const void* fb, int width, int height);
    int32_t (*set_power)(void* ctx, bool on);
    int32_t (*set_backlight)(void* ctx, uint8_t brightness);
    int32_t (*flush_partial)(void* ctx, const void* fb, int x, int y, int width, int height, int fb_width, int fb_height);
    bool (*supports_partial_update)(void* ctx);
    int32_t (*flush_async)(void* ctx, const void* fb, int width, int height,
                           void (*callback)(void* user_ctx, int32_t result), void* user_ctx);
    int32_t (*flush_partial_async)(void* ctx, const void* fb, int x, int y, int width, int height,
                                   int fb_width, int fb_height,
                                   void (*callback)(void* user_ctx, int32_t result), void* user_ctx);
    int32_t (*cancel_async)(void* ctx);
    bool (*is_async_busy)(void* ctx);
    uint32_t (*get_supported_refresh_modes)(void* ctx);
    int32_t (*start_continuous)(void* ctx, void* fb, int width, int height, const espio_display_refresh_config_t* config);
    int32_t (*update_framebuffer)(void* ctx, void* fb);
    int32_t (*stop_continuous)(void* ctx);
    int32_t (*wait_vsync)(void* ctx, uint32_t timeout_ms);
    int32_t (*get_caps)(void* ctx, espio_display_caps_t* caps);
    int32_t (*set_rotation)(void* ctx, espio_display_rotation_t rotation);
    espio_display_rotation_t (*get_rotation)(void* ctx);
    int32_t (*set_power_state)(void* ctx, espio_display_power_state_t state);
    espio_display_power_state_t (*get_power_state)(void* ctx);
    int32_t (*send_command)(void* ctx, uint32_t command,
                            const void* params, size_t param_len,
                            void* response, size_t* response_len);
} espio_display_driver_t;

/**
 * @brief Software buffering mode.
 */
typedef enum {
    ESPIO_DISPLAY_BUFFER_NONE = 0,
    ESPIO_DISPLAY_BUFFER_SINGLE = 1,
    ESPIO_DISPLAY_BUFFER_DOUBLE = 2,
} espio_display_buffer_mode_t;

/**
 * @brief Display configuration.
 */
typedef struct {
    const espio_display_driver_t* driver;
    const void* driver_config;
    espio_display_buffer_mode_t buffer_mode;
    bool use_psram;
} espio_display_config_t;

int32_t espio_display_set_power(espio_display_t* display, bool on);
int32_t espio_display_set_backlight(espio_display_t* display, uint8_t brightness);
espio_display_t* espio_display_create(const espio_display_config_t* config);
void espio_display_destroy(espio_display_t* display);
espio_display_pixel_format_t espio_display_get_pixel_format(espio_display_t* display);
size_t espio_display_get_bytes_per_pixel(espio_display_t* display);
size_t espio_display_get_bits_per_pixel(espio_display_t* display);
int32_t espio_display_clear(espio_display_t* display, uint64_t color);
int32_t espio_display_draw_bitmap(espio_display_t* display, int x, int y, int width, int height, const void* data);
int32_t espio_display_fill_rect(espio_display_t* display, int x, int y, int width, int height, uint64_t color);
int32_t espio_display_set_pixel(espio_display_t* display, int x, int y, uint64_t color);
int32_t espio_display_get_pixel(espio_display_t* display, int x, int y, uint64_t* color);
int32_t espio_display_get_width(espio_display_t* display);
int32_t espio_display_get_height(espio_display_t* display);
void* espio_display_get_framebuffer(espio_display_t* display);
int32_t espio_display_flush(espio_display_t* display);
int32_t espio_display_flush_partial(espio_display_t* display, int x, int y, int width, int height);
bool espio_display_supports_partial_update(espio_display_t* display);
int32_t espio_display_flush_async(espio_display_t* display, espio_display_async_callback_t callback, void* user_ctx);
int32_t espio_display_flush_partial_async(espio_display_t* display, int x, int y, int width, int height,
                                          espio_display_async_callback_t callback, void* user_ctx);
int32_t espio_display_wait_async(espio_display_t* display, int32_t timeout_ms);
int32_t espio_display_cancel_async(espio_display_t* display);
bool espio_display_is_async_busy(espio_display_t* display);
espio_display_refresh_mode_t espio_display_get_refresh_mode(espio_display_t* display);
bool espio_display_supports_continuous_refresh(espio_display_t* display);
int32_t espio_display_start_continuous(espio_display_t* display, const espio_display_refresh_config_t* config);
int32_t espio_display_present_framebuffer(espio_display_t* display);
int32_t espio_display_stop_continuous(espio_display_t* display);
int32_t espio_display_wait_vsync(espio_display_t* display, uint32_t timeout_ms);
bool espio_display_has_framebuffer(espio_display_t* display);
espio_display_buffer_mode_t espio_display_get_buffer_mode(espio_display_t* display);
bool espio_display_is_double_buffered(espio_display_t* display);
int32_t espio_display_swap_buffers(espio_display_t* display);
int32_t espio_display_wait_for_swap(espio_display_t* display, int32_t timeout_ms);
int32_t espio_display_get_caps(espio_display_t* display, espio_display_caps_t* caps);
int32_t espio_display_set_rotation(espio_display_t* display, espio_display_rotation_t rotation);
espio_display_rotation_t espio_display_get_rotation(espio_display_t* display);
int32_t espio_display_set_power_state(espio_display_t* display, espio_display_power_state_t state);
espio_display_power_state_t espio_display_get_power_state(espio_display_t* display);
int32_t espio_display_send_command(espio_display_t* display, uint32_t command,
                                   const void* params, size_t param_len,
                                   void* response, size_t* response_len);

/**
 * @brief RGB565-specific helper for `espio_display_draw_bitmap`.
 */
static inline int32_t espio_display_draw_bitmap_rgb565(espio_display_t* display,
                                                       int x,
                                                       int y,
                                                       int width,
                                                       int height,
                                                       const uint16_t* data) {
    return espio_display_draw_bitmap(display, x, y, width, height, (const void*)data);
}

/**
 * @brief RGB565-specific helper for `espio_display_fill_rect`.
 */
static inline int32_t espio_display_fill_rect_rgb565(espio_display_t* display,
                                                     int x,
                                                     int y,
                                                     int width,
                                                     int height,
                                                     uint16_t color) {
    return espio_display_fill_rect(display, x, y, width, height, (uint64_t)color);
}

/**
 * @brief RGB565-specific helper for `espio_display_set_pixel`.
 */
static inline int32_t espio_display_set_pixel_rgb565(espio_display_t* display, int x, int y, uint16_t color) {
    return espio_display_set_pixel(display, x, y, (uint64_t)color);
}

/**
 * @brief RGB565-specific helper for `espio_display_get_pixel`.
 */
static inline int32_t espio_display_get_pixel_rgb565(espio_display_t* display, int x, int y, uint16_t* color) {
    uint64_t value = 0U;
    int32_t result = espio_display_get_pixel(display, x, y, &value);
    if (result == ESPIO_DISPLAY_OK && color) {
        *color = (uint16_t)value;
    }
    return result;
}

/**
 * @brief RGB565-specific helper for `espio_display_clear`.
 */
static inline int32_t espio_display_clear_rgb565(espio_display_t* display, uint16_t color) {
    return espio_display_clear(display, (uint64_t)color);
}

/**
 * @brief Return the framebuffer as RGB565 when the format matches.
 */
static inline uint16_t* espio_display_get_framebuffer_rgb565(espio_display_t* display) {
    if (display && espio_display_get_pixel_format(display) == ESPIO_DISPLAY_PIXEL_FORMAT_RGB565) {
        return (uint16_t*)espio_display_get_framebuffer(display);
    }
    return NULL;
}

/**
 * @brief Pack RGB888 into RGB565.
 */
static inline uint16_t espio_display_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8U) << 8U) | ((g & 0xFCU) << 3U) | (b >> 3U));
}

/**
 * @brief Expand RGB565 into RGB888.
 */
static inline void espio_display_rgb888(uint16_t rgb565, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (!r || !g || !b) {
        return;
    }

    *r = (uint8_t)((rgb565 >> 8U) & 0xF8U);
    *g = (uint8_t)((rgb565 >> 3U) & 0xFCU);
    *b = (uint8_t)((rgb565 << 3U) & 0xF8U);
    *r |= (uint8_t)(*r >> 5U);
    *g |= (uint8_t)(*g >> 6U);
    *b |= (uint8_t)(*b >> 5U);
}

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_DISPLAY_H */
