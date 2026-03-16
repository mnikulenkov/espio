/**
 * @file ili9341.c
 * @brief ILI9341 Display Driver Implementation
 */

#include "espio/drivers/display_ili9341.h"
#include "espio/display_err.h"
#include "espio/log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char* TAG = "ili9341";

/**
 * @brief Swap bytes in a16-bit value (for RGB565 byte order correction)
 */
static inline uint16_t swap_bytes_uint16(uint16_t val) {
    return (val >>8) | (val <<8);
}

/**
 * @brief ILI9341 driver context (internal)
 */
typedef struct {
    esp_lcd_panel_handle_t panel;           ///< ESP LCD panel handle
    esp_lcd_panel_io_handle_t io;           ///< ESP LCD panel IO handle
    spi_host_device_t spi_host;             ///< SPI host device
    uint16_t width;                         ///< Display width
    uint16_t height;                        ///< Display height
    int backlight_pin;                      ///< Backlight pin
    ledc_mode_t backlight_mode;             ///< LEDC mode
    ledc_channel_t backlight_channel;       ///< LEDC channel
    bool backlight_active_low;              ///< Backlight active low flag
    bool backlight_initialized;             ///< Backlight initialized flag
    // Persistent swap buffer for DMA transfers (prevents use-after-free)
    uint16_t* swap_buffer;                  ///< Persistent buffer for byte-swapped data
    size_t swap_buffer_size;                ///< Size of swap buffer in bytes
} ili9341_context_t;

/* Forward declarations of driver functions */
static void* ili9341_driver_init(const void* config, uint16_t* width, uint16_t* height);
static void ili9341_driver_deinit(void* ctx);
static espio_display_pixel_format_t ili9341_driver_get_pixel_format(void* ctx);
static int32_t ili9341_driver_set_power(void* ctx, bool on);
static int32_t ili9341_driver_set_backlight(void* ctx, uint8_t brightness);
static int32_t ili9341_driver_draw_bitmap(void* ctx, int x, int y, int width, int height, const void* data);
static int32_t ili9341_driver_fill_rect(void* ctx, int x, int y, int width, int height, uint64_t color);
static int32_t ili9341_driver_flush(void* ctx, const void* fb, int width, int height);

/**
 * @brief ILI9341 driver definition
 */
static const espio_display_driver_t ili9341_driver = {
    .name = "ILI9341",
    .init = ili9341_driver_init,
    .deinit = ili9341_driver_deinit,
    .get_pixel_format = ili9341_driver_get_pixel_format,
    .set_power = ili9341_driver_set_power,
    .set_backlight = ili9341_driver_set_backlight,
    .draw_bitmap = ili9341_driver_draw_bitmap,
    .fill_rect = ili9341_driver_fill_rect,
    .flush = ili9341_driver_flush,
};

const espio_display_driver_t* espio_display_driver_ili9341(void) {
    return &ili9341_driver;
}

static void* ili9341_driver_init(const void* config, uint16_t* width, uint16_t* height) {
    const espio_display_ili9341_config_t* cfg = (const espio_display_ili9341_config_t*)config;
    
    if (!cfg || !width || !height) {
        ESPIO_LOGE(TAG, "Invalid parameters");
        return NULL;
    }

    ili9341_context_t* ctx = heap_caps_calloc(1, sizeof(ili9341_context_t), MALLOC_CAP_DEFAULT);
    if (!ctx) {
        ESPIO_LOGE(TAG, "Failed to allocate context");
        return NULL;
    }

    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->backlight_pin = cfg->backlight.pin;
    ctx->backlight_active_low = cfg->backlight.active_low;
    ctx->backlight_initialized = false;

    // Determine SPI host
    // SPI1_HOST is reserved for internal flash - only SPI2 and SPI3 are available
    spi_host_device_t spi_host;
    switch (cfg->spi.spi_host_id) {
        case 2:  spi_host = SPI2_HOST; break;
        case 3:  spi_host = SPI3_HOST; break;
        default:
            ESPIO_LOGE(TAG, "Invalid SPI host ID: %d (use 2 for SPI2 or 3 for SPI3)", cfg->spi.spi_host_id);
            goto error;
    }
    ctx->spi_host = spi_host;

    // Calculate max transfer size with smart auto-default
    uint32_t max_xfer = cfg->spi.max_transfer_size;
    if (max_xfer == ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_AUTO) {
        // Auto-calculate: use smaller dimension × 64 lines × 2 bytes, capped at 32KB
        // This adapts to orientation changes (swap_xy) and different display resolutions
        uint16_t min_dim = (cfg->width < cfg->height) ? cfg->width : cfg->height;
        max_xfer = min_dim * 64 * sizeof(uint16_t);
        if (max_xfer > ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_DEFAULT) {
            max_xfer = ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_DEFAULT;
        }
        if (max_xfer < ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_MIN) {
            max_xfer = ESPIO_DISPLAY_ILI9341_MAX_TRANSFER_MIN;
        }
    }
    ESPIO_LOGD(TAG, "SPI max transfer size: %lu bytes", max_xfer);

    // Initialize SPI bus
    spi_bus_config_t buscfg = {
        .sclk_io_num = cfg->spi.pin_sck,
        .mosi_io_num = cfg->spi.pin_mosi,
        .miso_io_num = cfg->spi.pin_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = max_xfer,
    };

    esp_err_t ret = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESPIO_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        goto error;
    }

    // Initialize panel IO
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = cfg->spi.pin_dc,
        .cs_gpio_num = cfg->spi.pin_cs,
        .pclk_hz = cfg->spi.spi_clock_hz,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = cfg->spi.spi_mode,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &ctx->io);
    if (ret != ESP_OK) {
        ESPIO_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        goto error_bus;
    }

    // Initialize ILI9341 panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = cfg->spi.pin_rst,
        .rgb_ele_order = cfg->orientation.rgb_order ? LCD_RGB_ELEMENT_ORDER_RGB : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = NULL,
    };

    ret = esp_lcd_new_panel_ili9341(ctx->io, &panel_config, &ctx->panel);
    if (ret != ESP_OK) {
        ESPIO_LOGE(TAG, "Failed to create ILI9341 panel: %s", esp_err_to_name(ret));
        goto error_io;
    }

    // Reset and initialize panel
    ret = esp_lcd_panel_reset(ctx->panel);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESPIO_LOGE(TAG, "Failed to reset panel: %s", esp_err_to_name(ret));
        goto error_panel;
    }

    ret = esp_lcd_panel_init(ctx->panel);
    if (ret != ESP_OK) {
        ESPIO_LOGE(TAG, "Failed to init panel: %s", esp_err_to_name(ret));
        goto error_panel;
    }

    // Turn on display
    ret = esp_lcd_panel_disp_on_off(ctx->panel, true);
    if (ret != ESP_OK) {
        ESPIO_LOGE(TAG, "Failed to turn on display: %s", esp_err_to_name(ret));
        goto error_panel;
    }

    // Set orientation
    if (cfg->orientation.mirror_x || cfg->orientation.mirror_y) {
        esp_lcd_panel_mirror(ctx->panel, cfg->orientation.mirror_x, cfg->orientation.mirror_y);
    }
    if (cfg->orientation.swap_xy) {
        esp_lcd_panel_swap_xy(ctx->panel, true);
    }

    // Initialize backlight if configured
    if (cfg->backlight.pin >= 0) {
        ledc_timer_config_t tcfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = LEDC_TIMER_0,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .freq_hz = cfg->backlight.pwm_freq_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ret = ledc_timer_config(&tcfg);
        if (ret != ESP_OK) {
            ESPIO_LOGE(TAG, "Failed to config LEDC timer: %s", esp_err_to_name(ret));
            goto error_panel;
        }

        ctx->backlight_mode = LEDC_LOW_SPEED_MODE;
        ctx->backlight_channel = LEDC_CHANNEL_0;

        ledc_channel_config_t ccfg = {
            .gpio_num = cfg->backlight.pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = ctx->backlight_active_low ? (255 - cfg->initial_brightness) : cfg->initial_brightness,
            .hpoint = 0,
        };
        ret = ledc_channel_config(&ccfg);
        if (ret != ESP_OK) {
            ESPIO_LOGE(TAG, "Failed to config LEDC channel: %s", esp_err_to_name(ret));
            goto error_panel;
        }
        ret = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        if (ret != ESP_OK) {
            ESPIO_LOGE(TAG, "Failed to update LEDC duty: %s", esp_err_to_name(ret));
            goto error_panel;
        }
        ctx->backlight_initialized = true;
        ESPIO_LOGI(TAG, "Backlight initialized on pin %d", cfg->backlight.pin);
    }

    *width = ctx->width;
    *height = ctx->height;

    // Allocate persistent swap buffer for DMA transfers
    // This prevents use-after-free issues with async DMA
    size_t fb_size = ctx->width * ctx->height * sizeof(uint16_t);
    ctx->swap_buffer = heap_caps_malloc(fb_size, MALLOC_CAP_DMA | MALLOC_CAP_32BIT);
    if (ctx->swap_buffer) {
        ctx->swap_buffer_size = fb_size;
        ESPIO_LOGI(TAG, "Swap buffer allocated: %d bytes", fb_size);
    } else {
        ctx->swap_buffer_size = 0;
        ESPIO_LOGW(TAG, "Failed to allocate swap buffer, will use direct draw");
    }

    ESPIO_LOGI(TAG, "ILI9341 initialized: %dx%d", ctx->width, ctx->height);
    return ctx;

error_panel:
    esp_lcd_panel_del(ctx->panel);
error_io:
    esp_lcd_panel_io_del(ctx->io);
error_bus:
    spi_bus_free(spi_host);
error:
    heap_caps_free(ctx);
    return NULL;
}

static void ili9341_driver_deinit(void* ctx) {
    if (!ctx) return;

    ili9341_context_t* ili_ctx = (ili9341_context_t*)ctx;

    if (ili_ctx->panel) {
        esp_lcd_panel_disp_on_off(ili_ctx->panel, false);
        esp_lcd_panel_del(ili_ctx->panel);
    }

    if (ili_ctx->io) {
        esp_lcd_panel_io_del(ili_ctx->io);
    }

    spi_bus_free(ili_ctx->spi_host);

    // Free persistent swap buffer
    if (ili_ctx->swap_buffer) {
        heap_caps_free(ili_ctx->swap_buffer);
        ili_ctx->swap_buffer = NULL;
    }

    heap_caps_free(ili_ctx);
}

static int32_t ili9341_driver_set_power(void* ctx, bool on) {
    if (!ctx) return ESPIO_INVALID_ARG;

    ili9341_context_t* ili_ctx = (ili9341_context_t*)ctx;
    esp_err_t ret = esp_lcd_panel_disp_on_off(ili_ctx->panel, on);
    return (ret == ESP_OK) ? ESPIO_DISPLAY_OK : ESPIO_DISPLAY_ERR_DRIVER_FAILED;
}

static int32_t ili9341_driver_set_backlight(void* ctx, uint8_t brightness) {
    if (!ctx) return ESPIO_INVALID_ARG;

    ili9341_context_t* ili_ctx = (ili9341_context_t*)ctx;
    
    if (!ili_ctx->backlight_initialized) return ESPIO_DISPLAY_ERR_BACKLIGHT;

    uint32_t duty = ili_ctx->backlight_active_low ? (255 - brightness) : brightness;
    esp_err_t ret = ledc_set_duty(ili_ctx->backlight_mode, ili_ctx->backlight_channel, duty);
    if (ret != ESP_OK) return ESPIO_DISPLAY_ERR_BACKLIGHT;

    ret = ledc_update_duty(ili_ctx->backlight_mode, ili_ctx->backlight_channel);
    return (ret == ESP_OK) ? ESPIO_DISPLAY_OK : ESPIO_DISPLAY_ERR_BACKLIGHT;
}

static espio_display_pixel_format_t ili9341_driver_get_pixel_format(void* ctx) {
    // ILI9341 uses RGB565 format
    return ESPIO_DISPLAY_PIXEL_FORMAT_RGB565;
}

static int32_t ili9341_driver_draw_bitmap(void* ctx, int x, int y, int width, int height, const void* data) {
    if (!ctx || !data) return ESPIO_INVALID_ARG;

    ili9341_context_t* ili_ctx = (ili9341_context_t*)ctx;
    // ILI9341 uses RGB565, so cast to uint16_t*
    const uint16_t* rgb565_data = (const uint16_t*)data;
    esp_err_t ret = esp_lcd_panel_draw_bitmap(ili_ctx->panel, x, y, x + width, y + height, rgb565_data);
    return (ret == ESP_OK) ? ESPIO_DISPLAY_OK : ESPIO_DISPLAY_ERR_SPI_WRITE;
}

static int32_t ili9341_driver_fill_rect(void* ctx, int x, int y, int width, int height, uint64_t color) {
    if (!ctx) return ESPIO_INVALID_ARG;

    ili9341_context_t* ili_ctx = (ili9341_context_t*)ctx;

    // Allocate a line buffer for filling
    uint16_t* line = heap_caps_malloc(width * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line) return ESPIO_NO_MEM;

    // Swap bytes in color for correct RGB565 display
    // Color is passed as uint64_t but ILI9341 uses RGB565 (16-bit)
    uint16_t color16 = (uint16_t)(color & 0xFFFF);
    uint16_t swapped_color = swap_bytes_uint16(color16);
    
    for (int i = 0; i < width; i++) {
        line[i] = swapped_color;
    }

    esp_err_t ret = ESP_OK;
    for (int row = 0; row < height && ret == ESP_OK; row++) {
        ret = esp_lcd_panel_draw_bitmap(ili_ctx->panel, x, y + row, x + width, y + row + 1, line);
    }

    heap_caps_free(line);
    return (ret == ESP_OK) ? ESPIO_DISPLAY_OK : ESPIO_DISPLAY_ERR_SPI_WRITE;
}

static int32_t ili9341_driver_flush(void* ctx, const void* fb, int width, int height) {
    if (!ctx || !fb) return ESPIO_INVALID_ARG;

    ili9341_context_t* ili_ctx = (ili9341_context_t*)ctx;
    // ILI9341 uses RGB565, so cast to uint16_t*
    const uint16_t* fb16 = (const uint16_t*)fb;
    size_t fb_size = width * height * sizeof(uint16_t);
    
    // Use persistent swap buffer if available and correctly sized
    uint16_t* swap_buf = NULL;
    bool use_persistent = (ili_ctx->swap_buffer && ili_ctx->swap_buffer_size >= fb_size);
    
    if (use_persistent) {
        swap_buf = ili_ctx->swap_buffer;
    } else {
        // Fallback: allocate temporary buffer
        swap_buf = heap_caps_malloc(fb_size, MALLOC_CAP_DMA | MALLOC_CAP_32BIT);
        if (!swap_buf) {
            // Last resort: draw without byte swapping (may have incorrect colors)
            ESPIO_LOGW(TAG, "No swap buffer, direct draw (colors may be swapped)");
            esp_err_t ret = esp_lcd_panel_draw_bitmap(ili_ctx->panel, 0, 0, width, height, fb16);
            return (ret == ESP_OK) ? ESPIO_DISPLAY_OK : ESPIO_DISPLAY_ERR_SPI_WRITE;
        }
    }
    
    // Swap bytes in the entire framebuffer
    for (int i = 0; i < width * height; i++) {
        swap_buf[i] = swap_bytes_uint16(fb16[i]);
    }
    
    // Draw bitmap - this starts an async DMA transfer from swap_buf
    esp_err_t ret = esp_lcd_panel_draw_bitmap(ili_ctx->panel, 0, 0, width, height, swap_buf);
    
    // CRITICAL: Wait for DMA transfer to complete before returning
    // This prevents swap buffer corruption when next flush re called
    if (ret == ESP_OK) {
        // Wait for DMA transaction to complete
        // Use a dummy synchronous transaction to ensure DMA is done
        esp_lcd_panel_io_tx_param(ili_ctx->io, 0, NULL, 0);
    }
    
    // Free temporary buffer if allocated
    if (!use_persistent && swap_buf) {
        heap_caps_free(swap_buf);
    }
    
    return (ret == ESP_OK) ? ESPIO_DISPLAY_OK : ESPIO_DISPLAY_ERR_SPI_WRITE;
}
