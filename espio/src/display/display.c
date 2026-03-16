/**
 * @file display.c
 * @brief Display Abstraction Layer Implementation
 */

#include "espio/display.h"
#include "espio/display_err.h"
#include "espio/log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG = "display";

/**
 * @brief Helper function to allocate a framebuffer
 */
static void* allocate_framebuffer(size_t size, bool prefer_psram) {
    void* buffer = NULL;
    
    if (prefer_psram) {
        // Try PSRAM first (larger capacity, slower access)
        // Note: PSRAM may not support DMA, so we need to copy to DMA buffer for transfers
        buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffer) {
            ESPIO_LOGI(TAG, "Framebuffer allocated from PSRAM: %d bytes", size);
            return buffer;
        }
        ESPIO_LOGW(TAG, "PSRAM allocation failed, falling back to internal RAM");
    }
    
    // Fall back to internal RAM with DMA capability and 32-bit alignment
    // MALLOC_CAP_32BIT ensures proper alignment for DMA transfers
    buffer = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_32BIT);
    if (buffer) {
        ESPIO_LOGI(TAG, "Framebuffer allocated from internal RAM (DMA/32bit): %d bytes", size);
    }
    
    return buffer;
}

/**
 * @brief Display structure (internal)
 */
struct espio_display {
    const espio_display_driver_t* driver; ///< Driver interface
    void* driver_ctx;                   ///< Driver-specific context
    uint16_t width;                     ///< Display width
    uint16_t height;                    ///< Display height
    espio_display_buffer_mode_t buffer_mode; ///< Buffering mode
    espio_display_pixel_format_t format; ///< Pixel format
    
    // Buffer management (supports up to 2 buffers for double buffering)
    void* framebuffers[2];              ///< Array of framebuffer pointers (format-agnostic)
    size_t framebuffer_size;            ///< Size of each framebuffer in bytes
    uint8_t front_buffer_index;         ///< Which buffer is currently displayed
    uint8_t back_buffer_index;          ///< Which buffer is being drawn to
    uint8_t buffer_count;               ///< Number of allocated buffers
    
    // Synchronization for double buffering
    volatile bool flush_in_progress;    ///< DMA transfer in progress flag
    SemaphoreHandle_t flush_complete;   ///< Signal for async flush completion
    bool use_psram;                     ///< PSRAM allocation flag
    
    // Async operation state (Phase 3)
    espio_display_async_state_t async_state; ///< Current async operation state
    int32_t last_async_result;              ///< Result of last async operation
    espio_display_async_callback_t user_callback; ///< User callback for async ops
    void* user_ctx;                         ///< User context for callback
    
    // Continuous refresh state (Phase 4)
    espio_display_refresh_mode_t refresh_mode; ///< Current refresh mode
    bool continuous_active;                 ///< Continuous refresh is active
    void* active_fb;                        ///< Framebuffer being displayed
};

/**
 * @brief Get pointer to the current back buffer
 */
static inline void* get_back_buffer(espio_display_t* display) {
    return display->framebuffers[display->back_buffer_index];
}

/**
 * @brief Check if any framebuffer is allocated
 */
static inline bool has_framebuffer(espio_display_t* display) {
    return display->buffer_mode != ESPIO_DISPLAY_BUFFER_NONE && display->buffer_count > 0;
}

espio_display_t* espio_display_create(const espio_display_config_t* config) {
    if (!config || !config->driver) {
        ESPIO_LOGE(TAG, "Invalid configuration or driver");
        return NULL;
    }

    // Allocate display structure
    espio_display_t* disp = heap_caps_calloc(1, sizeof(espio_display_t), MALLOC_CAP_DEFAULT);
    if (!disp) {
        ESPIO_LOGE(TAG, "Failed to allocate display structure");
        return NULL;
    }

    disp->driver = config->driver;
    disp->buffer_mode = config->buffer_mode;
    disp->use_psram = config->use_psram;
    disp->front_buffer_index = 0;
    disp->back_buffer_index = 0;
    disp->buffer_count = 0;
    disp->flush_in_progress = false;
    disp->format = ESPIO_DISPLAY_PIXEL_FORMAT_RGB565;  // Default, will be updated from driver

    // Initialize driver
    uint16_t width = 0, height = 0;
    disp->driver_ctx = config->driver->init(config->driver_config, &width, &height);
    if (!disp->driver_ctx) {
        ESPIO_LOGE(TAG, "Driver initialization failed");
        heap_caps_free(disp);
        return NULL;
    }

    disp->width = width;
    disp->height = height;
    
    // Get pixel format from driver
    if (config->driver->get_pixel_format) {
        disp->format = config->driver->get_pixel_format(disp->driver_ctx);
    } else {
        // Default to RGB565 for backward compatibility with old drivers
        disp->format = ESPIO_DISPLAY_PIXEL_FORMAT_RGB565;
        ESPIO_LOGW(TAG, "Driver does not implement get_pixel_format, assuming RGB565");
    }

    // Calculate framebuffer size based on pixel format
    size_t bits_per_pixel = espio_display_format_bits_per_pixel(disp->format);
    size_t fb_bits = (size_t)width * (size_t)height * bits_per_pixel;
    size_t fb_size = (fb_bits + 7) / 8;  // Round up to bytes
    disp->framebuffer_size = fb_size;
    
    ESPIO_LOGI(TAG, "Pixel format: %d, bits/pixel: %d, framebuffer size: %d bytes",
             disp->format, bits_per_pixel, fb_size);
    
    switch (disp->buffer_mode) {
        case ESPIO_DISPLAY_BUFFER_DOUBLE:
            // Allocate two framebuffers for double buffering
            disp->framebuffers[0] = allocate_framebuffer(fb_size, disp->use_psram);
            if (disp->framebuffers[0]) {
                disp->buffer_count = 1;
                memset(disp->framebuffers[0], 0, fb_size);
                
                disp->framebuffers[1] = allocate_framebuffer(fb_size, disp->use_psram);
                if (disp->framebuffers[1]) {
                    disp->buffer_count = 2;
                    memset(disp->framebuffers[1], 0, fb_size);
                    
                    // Create synchronization semaphore for double buffering
                    disp->flush_complete = xSemaphoreCreateBinary();
                    if (!disp->flush_complete) {
                        ESPIO_LOGW(TAG, "Failed to create flush semaphore, using sync mode");
                    }
                    
                    ESPIO_LOGI(TAG, "Double buffering enabled: 2 framebuffers allocated");
                } else {
                    ESPIO_LOGW(TAG, "Failed to allocate second framebuffer, falling back to single buffer");
                    disp->buffer_mode = ESPIO_DISPLAY_BUFFER_SINGLE;
                }
            } else {
                ESPIO_LOGW(TAG, "Failed to allocate any framebuffer, using direct mode");
                disp->buffer_mode = ESPIO_DISPLAY_BUFFER_NONE;
            }
            break;
            
        case ESPIO_DISPLAY_BUFFER_SINGLE:
            // Allocate single framebuffer
            disp->framebuffers[0] = allocate_framebuffer(fb_size, disp->use_psram);
            if (disp->framebuffers[0]) {
                disp->buffer_count = 1;
                memset(disp->framebuffers[0], 0, fb_size);
                ESPIO_LOGI(TAG, "Single buffer mode: 1 framebuffer allocated");
            } else {
                ESPIO_LOGW(TAG, "Failed to allocate framebuffer, using direct mode");
                disp->buffer_mode = ESPIO_DISPLAY_BUFFER_NONE;
            }
            break;
            
        case ESPIO_DISPLAY_BUFFER_NONE:
        default:
            ESPIO_LOGI(TAG, "Direct mode: no framebuffer allocated");
            break;
    }

    const char* mode_str = "unknown";
    switch (disp->buffer_mode) {
        case ESPIO_DISPLAY_BUFFER_NONE: mode_str = "direct"; break;
        case ESPIO_DISPLAY_BUFFER_SINGLE: mode_str = "single"; break;
        case ESPIO_DISPLAY_BUFFER_DOUBLE: mode_str = "double"; break;
    }

    ESPIO_LOGI(TAG, "Display created: %s, %dx%d, format=%d, buffer=%s (%d buffers)",
             disp->driver->name,
             disp->width,
             disp->height,
             disp->format,
             mode_str,
             disp->buffer_count);

    return disp;
}

void espio_display_destroy(espio_display_t* display) {
    if (!display) return;

    // Wait for any pending flush to complete
    if (display->flush_in_progress && display->flush_complete) {
        xSemaphoreTake(display->flush_complete, pdMS_TO_TICKS(1000));
    }

    // Free framebuffers
    for (int i = 0; i < 2; i++) {
        if (display->framebuffers[i]) {
            heap_caps_free(display->framebuffers[i]);
            display->framebuffers[i] = NULL;
        }
    }

    // Free semaphore
    if (display->flush_complete) {
        vSemaphoreDelete(display->flush_complete);
        display->flush_complete = NULL;
    }

    // Deinitialize driver
    if (display->driver && display->driver->deinit) {
        display->driver->deinit(display->driver_ctx);
    }

    heap_caps_free(display);
}

int32_t espio_display_set_power(espio_display_t* display, bool on) {
    if (!display || !display->driver || !display->driver->set_power) {
        return ESPIO_INVALID_ARG;
    }
    return display->driver->set_power(display->driver_ctx, on);
}

int32_t espio_display_set_backlight(espio_display_t* display, uint8_t brightness) {
    if (!display || !display->driver || !display->driver->set_backlight) {
        return ESPIO_INVALID_ARG;
    }
    return display->driver->set_backlight(display->driver_ctx, brightness);
}

/*============================================================================
 * Pixel Format Query Functions
 *============================================================================*/

espio_display_pixel_format_t espio_display_get_pixel_format(espio_display_t* display) {
    if (!display) return ESPIO_DISPLAY_PIXEL_FORMAT_RGB565;
    return display->format;
}

size_t espio_display_get_bytes_per_pixel(espio_display_t* display) {
    if (!display) return 2;  // Default RGB565
    return espio_display_format_bytes_per_pixel(display->format);
}

size_t espio_display_get_bits_per_pixel(espio_display_t* display) {
    if (!display) return 16;  // Default RGB565
    return espio_display_format_bits_per_pixel(display->format);
}

/*============================================================================
 * Drawing Functions
 *============================================================================*/

int32_t espio_display_clear(espio_display_t* display, uint64_t color) {
    if (!display) return ESPIO_INVALID_ARG;
    return espio_display_fill_rect(display, 0, 0, display->width, display->height, color);
}

int32_t espio_display_draw_bitmap(espio_display_t* display, int x, int y, int width, int height, const void* data) {
    if (!display || !data) return ESPIO_INVALID_ARG;

    // Clip to display bounds
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > display->width) width = display->width - x;
    if (y + height > display->height) height = display->height - y;
    if (width <= 0 || height <= 0) return ESPIO_DISPLAY_OK;

    if (has_framebuffer(display)) {
        void* fb = get_back_buffer(display);
        size_t bytes_per_pixel = espio_display_format_bytes_per_pixel(display->format);
        
        // Handle different pixel formats
        if (bytes_per_pixel > 0) {
            // Non-packed formats (RGB565, RGB888, grayscale8, indexed8)
            uint8_t* fb_bytes = (uint8_t*)fb;
            const uint8_t* src_bytes = (const uint8_t*)data;
            
            for (int row = 0; row < height; row++) {
                int fb_y = y + row;
                if (fb_y >= display->height) break;
                
                size_t fb_offset = (fb_y * display->width + x) * bytes_per_pixel;
                size_t src_offset = row * width * bytes_per_pixel;
                
                memcpy(&fb_bytes[fb_offset], &src_bytes[src_offset], width * bytes_per_pixel);
            }
        } else {
            // Packed formats (MONO, GRAYSCALE_4) - byte-level operations
            // For now, fall back to driver for packed formats
            if (display->driver && display->driver->draw_bitmap) {
                return display->driver->draw_bitmap(display->driver_ctx, x, y, width, height, data);
            }
            return ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT;
        }
        return ESPIO_DISPLAY_OK;
    }

    // Direct draw
    if (display->driver && display->driver->draw_bitmap) {
        return display->driver->draw_bitmap(display->driver_ctx, x, y, width, height, data);
    }

    return ESPIO_DISPLAY_ERR_DRIVER_FAILED;
}

int32_t espio_display_fill_rect(espio_display_t* display, int x, int y, int width, int height, uint64_t color) {
    if (!display) return ESPIO_INVALID_ARG;

    // Clip to display bounds
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > display->width) width = display->width - x;
    if (y + height > display->height) height = display->height - y;
    if (width <= 0 || height <= 0) return ESPIO_DISPLAY_OK;

    if (has_framebuffer(display)) {
        void* fb = get_back_buffer(display);
        size_t bytes_per_pixel = espio_display_format_bytes_per_pixel(display->format);
        
        // Handle different pixel formats
        if (bytes_per_pixel > 0) {
            // Non-packed formats
            uint8_t* fb_bytes = (uint8_t*)fb;
            
            for (int row = 0; row < height; row++) {
                int fb_y = y + row;
                if (fb_y >= display->height) break;
                
                for (int col = 0; col < width; col++) {
                    int fb_x = x + col;
                    if (fb_x >= display->width) break;
                    
                    size_t offset = (fb_y * display->width + fb_x) * bytes_per_pixel;
                    
                    // Write color bytes (little-endian)
                    for (size_t b = 0; b < bytes_per_pixel; b++) {
                        fb_bytes[offset + b] = (color >> (b * 8)) & 0xFF;
                    }
                }
            }
        } else {
            // Packed formats - fall back to driver
            if (display->driver && display->driver->fill_rect) {
                return display->driver->fill_rect(display->driver_ctx, x, y, width, height, color);
            }
            return ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT;
        }
        return ESPIO_DISPLAY_OK;
    }

    // Direct fill
    if (display->driver && display->driver->fill_rect) {
        return display->driver->fill_rect(display->driver_ctx, x, y, width, height, color);
    }

    return ESPIO_DISPLAY_ERR_DRIVER_FAILED;
}

int32_t espio_display_set_pixel(espio_display_t* display, int x, int y, uint64_t color) {
    if (!display) return ESPIO_INVALID_ARG;
    if (x < 0 || x >= display->width || y < 0 || y >= display->height) {
        return ESPIO_INVALID_ARG;
    }

    if (has_framebuffer(display)) {
        void* fb = get_back_buffer(display);
        size_t bytes_per_pixel = espio_display_format_bytes_per_pixel(display->format);
        
        if (bytes_per_pixel > 0) {
            uint8_t* fb_bytes = (uint8_t*)fb;
            size_t offset = (y * display->width + x) * bytes_per_pixel;
            
            // Write color bytes (little-endian)
            for (size_t b = 0; b < bytes_per_pixel; b++) {
                fb_bytes[offset + b] = (color >> (b * 8)) & 0xFF;
            }
        } else {
            // Packed formats - fall back to driver
            if (display->driver && display->driver->fill_rect) {
                return display->driver->fill_rect(display->driver_ctx, x, y, 1, 1, color);
            }
            return ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT;
        }
        return ESPIO_DISPLAY_OK;
    }

    // Direct pixel set (inefficient but works)
    if (display->driver && display->driver->fill_rect) {
        return display->driver->fill_rect(display->driver_ctx, x, y, 1, 1, color);
    }

    return ESPIO_DISPLAY_ERR_DRIVER_FAILED;
}

int32_t espio_display_get_pixel(espio_display_t* display, int x, int y, uint64_t* color) {
    if (!display || !color) return ESPIO_INVALID_ARG;
    if (x < 0 || x >= display->width || y < 0 || y >= display->height) {
        return ESPIO_INVALID_ARG;
    }

    *color = 0;

    if (has_framebuffer(display)) {
        void* fb = get_back_buffer(display);
        size_t bytes_per_pixel = espio_display_format_bytes_per_pixel(display->format);
        
        if (bytes_per_pixel > 0) {
            uint8_t* fb_bytes = (uint8_t*)fb;
            size_t offset = (y * display->width + x) * bytes_per_pixel;
            
            // Read color bytes (little-endian)
            for (size_t b = 0; b < bytes_per_pixel; b++) {
                *color |= ((uint64_t)fb_bytes[offset + b]) << (b * 8);
            }
        } else {
            // Packed formats not supported for get_pixel in software
            return ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT;
        }
        return ESPIO_DISPLAY_OK;
    }

    // Cannot read from direct display without framebuffer
    return ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER;
}

/*============================================================================
 * Display Info Functions
 *============================================================================*/

int32_t espio_display_get_width(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;
    return display->width;
}

int32_t espio_display_get_height(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;
    return display->height;
}

void* espio_display_get_framebuffer(espio_display_t* display) {
    if (!display) return NULL;
    return get_back_buffer(display);
}

int32_t espio_display_flush(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;

    if (!has_framebuffer(display)) {
        // No framebuffer, nothing to flush
        return ESPIO_DISPLAY_OK;
    }

    // In double buffer mode, recommend using swap_buffers instead
    if (display->buffer_mode == ESPIO_DISPLAY_BUFFER_DOUBLE) {
        ESPIO_LOGW(TAG, "espio_display_flush() called in double buffer mode, use espio_display_swap_buffers() instead");
    }

    // Flush back buffer to driver
    if (display->driver && display->driver->flush) {
        void* fb = get_back_buffer(display);
        return display->driver->flush(display->driver_ctx, fb,
                                       display->width, display->height);
    }

    return ESPIO_DISPLAY_ERR_DRIVER_FAILED;
}

/*============================================================================
 * Partial Update Functions (Phase 2)
 *============================================================================*/

int32_t espio_display_flush_partial(espio_display_t* display, int x, int y, int width, int height) {
    if (!display) return ESPIO_INVALID_ARG;
    
    if (!has_framebuffer(display)) {
        return ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER;
    }
    
    // Clip to display bounds
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > display->width) width = display->width - x;
    if (y + height > display->height) height = display->height - y;
    if (width <= 0 || height <= 0) return ESPIO_DISPLAY_OK;
    
    void* fb = get_back_buffer(display);
    
    // Try driver partial flush first
    if (display->driver && display->driver->flush_partial) {
        return display->driver->flush_partial(display->driver_ctx, fb,
                                               x, y, width, height,
                                               display->width, display->height);
    }
    
    // Fall back to full flush
    if (display->driver && display->driver->flush) {
        return display->driver->flush(display->driver_ctx, fb,
                                       display->width, display->height);
    }
    
    return ESPIO_DISPLAY_ERR_DRIVER_FAILED;
}

bool espio_display_supports_partial_update(espio_display_t* display) {
    if (!display || !display->driver) return false;
    
    // Check explicit support function
    if (display->driver->supports_partial_update) {
        return display->driver->supports_partial_update(display->driver_ctx);
    }
    
    // If flush_partial exists, assume supported
    return display->driver->flush_partial != NULL;
}

/*============================================================================
 * Buffer Management Functions
 *============================================================================*/

/*============================================================================
 * Async Operation Functions (Phase 3)
 *============================================================================*/

/**
 * @brief Internal callback wrapper for driver async operations
 */
static void async_completion_handler(void* user_ctx, int32_t result) {
    espio_display_t* display = (espio_display_t*)user_ctx;
    if (!display) return;
    
    // Store result
    display->last_async_result = result;
    display->async_state = ESPIO_DISPLAY_ASYNC_IDLE;
    
    // Signal completion semaphore
    if (display->flush_complete) {
        xSemaphoreGive(display->flush_complete);
    }
    
    // Call user callback
    if (display->user_callback) {
        display->user_callback(display->user_ctx, result);
    }
}

int32_t espio_display_flush_async(espio_display_t* display,
                                  espio_display_async_callback_t callback, void* user_ctx) {
    if (!display) return ESPIO_INVALID_ARG;
    if (!has_framebuffer(display)) return ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER;
    
    // Check if already busy
    if (display->async_state != ESPIO_DISPLAY_ASYNC_IDLE) {
        return ESPIO_DISPLAY_ERR_ASYNC_BUSY;
    }
    
    // Store callback info
    display->user_callback = callback;
    display->user_ctx = user_ctx;
    display->async_state = ESPIO_DISPLAY_ASYNC_FLUSHING;
    
    void* fb = get_back_buffer(display);
    
    // Try driver async flush
    if (display->driver && display->driver->flush_async) {
        return display->driver->flush_async(display->driver_ctx, fb,
                                            display->width, display->height,
                                            async_completion_handler, display);
    }
    
    // Fall back to sync flush with immediate callback
    int32_t result = ESPIO_DISPLAY_ERR_DRIVER_FAILED;
    if (display->driver && display->driver->flush) {
        result = display->driver->flush(display->driver_ctx, fb,
                                        display->width, display->height);
    }
    
    // Complete immediately
    display->async_state = ESPIO_DISPLAY_ASYNC_IDLE;
    if (callback) {
        callback(user_ctx, result);
    }
    
    return result;
}

int32_t espio_display_flush_partial_async(espio_display_t* display,
                                          int x, int y, int width, int height,
                                          espio_display_async_callback_t callback, void* user_ctx) {
    if (!display) return ESPIO_INVALID_ARG;
    if (!has_framebuffer(display)) return ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER;
    
    // Check if already busy
    if (display->async_state != ESPIO_DISPLAY_ASYNC_IDLE) {
        return ESPIO_DISPLAY_ERR_ASYNC_BUSY;
    }
    
    // Clip to display bounds
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > display->width) width = display->width - x;
    if (y + height > display->height) height = display->height - y;
    if (width <= 0 || height <= 0) {
        if (callback) callback(user_ctx, ESPIO_DISPLAY_OK);
        return ESPIO_DISPLAY_OK;
    }
    
    // Store callback info
    display->user_callback = callback;
    display->user_ctx = user_ctx;
    display->async_state = ESPIO_DISPLAY_ASYNC_PARTIAL;
    
    void* fb = get_back_buffer(display);
    
    // Try driver async partial flush
    if (display->driver && display->driver->flush_partial_async) {
        return display->driver->flush_partial_async(display->driver_ctx, fb,
                                                     x, y, width, height,
                                                     display->width, display->height,
                                                     async_completion_handler, display);
    }
    
    // Fall back to sync partial flush
    int32_t result = espio_display_flush_partial(display, x, y, width, height);
    
    // Complete immediately
    display->async_state = ESPIO_DISPLAY_ASYNC_IDLE;
    if (callback) {
        callback(user_ctx, result);
    }
    
    return result;
}

int32_t espio_display_wait_async(espio_display_t* display, int32_t timeout_ms) {
    if (!display) return ESPIO_INVALID_ARG;
    
    // Not busy
    if (display->async_state == ESPIO_DISPLAY_ASYNC_IDLE) {
        return ESPIO_DISPLAY_OK;
    }
    
    // Wait for completion
    if (display->flush_complete) {
        TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        if (xSemaphoreTake(display->flush_complete, ticks) == pdTRUE) {
            return display->last_async_result;
        }
        return ESPIO_TIMEOUT;
    }
    
    return ESPIO_DISPLAY_OK;
}

int32_t espio_display_cancel_async(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;
    
    // Not busy
    if (display->async_state == ESPIO_DISPLAY_ASYNC_IDLE) {
        return ESPIO_DISPLAY_OK;
    }
    
    // Try driver cancel
    if (display->driver && display->driver->cancel_async) {
        int32_t result = display->driver->cancel_async(display->driver_ctx);
        if (result == ESPIO_DISPLAY_OK) {
            display->async_state = ESPIO_DISPLAY_ASYNC_CANCELLED;
        }
        return result;
    }
    
    // Can't cancel, wait for completion
    return espio_display_wait_async(display, 5000);
}

bool espio_display_is_async_busy(espio_display_t* display) {
    if (!display) return false;
    
    // Check driver first
    if (display->driver && display->driver->is_async_busy) {
        return display->driver->is_async_busy(display->driver_ctx);
    }
    
    // Use internal state
    return display->async_state != ESPIO_DISPLAY_ASYNC_IDLE;
}

/*============================================================================
 * Continuous Refresh Functions (Phase 4)
 *============================================================================*/

espio_display_refresh_mode_t espio_display_get_refresh_mode(espio_display_t* display) {
    if (!display) return ESPIO_DISPLAY_REFRESH_ON_DEMAND;
    
    // Check if driver supports continuous refresh
    if (display->driver && display->driver->get_supported_refresh_modes) {
        uint32_t modes = display->driver->get_supported_refresh_modes(display->driver_ctx);
        if (modes & (1 << ESPIO_DISPLAY_REFRESH_CONTINUOUS)) {
            return ESPIO_DISPLAY_REFRESH_CONTINUOUS;
        }
    }
    
    return ESPIO_DISPLAY_REFRESH_ON_DEMAND;
}

bool espio_display_supports_continuous_refresh(espio_display_t* display) {
    if (!display || !display->driver) return false;
    
    if (display->driver->get_supported_refresh_modes) {
        uint32_t modes = display->driver->get_supported_refresh_modes(display->driver_ctx);
        return (modes & (1 << ESPIO_DISPLAY_REFRESH_CONTINUOUS)) != 0;
    }
    
    // If start_continuous exists, assume supported
    return display->driver->start_continuous != NULL;
}

int32_t espio_display_start_continuous(espio_display_t* display, const espio_display_refresh_config_t* config) {
    if (!display) return ESPIO_INVALID_ARG;
    
    if (!display->driver || !display->driver->start_continuous) {
        return ESPIO_DISPLAY_ERR_CONTINUOUS_NOT_SUPPORTED;
    }
    
    if (!has_framebuffer(display)) {
        return ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER;
    }
    
    void* fb = get_back_buffer(display);
    
    int32_t result = display->driver->start_continuous(
        display->driver_ctx, fb, display->width, display->height, config);
    
    if (result == ESPIO_DISPLAY_OK) {
        display->continuous_active = true;
        display->active_fb = fb;
        display->refresh_mode = ESPIO_DISPLAY_REFRESH_CONTINUOUS;
    }
    
    return result;
}

int32_t espio_display_present_framebuffer(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;
    
    if (!display->continuous_active) {
        return ESPIO_DISPLAY_ERR_NOT_IN_CONTINUOUS_MODE;
    }
    
    // Wait for vsync if supported
    if (display->driver && display->driver->wait_vsync) {
        display->driver->wait_vsync(display->driver_ctx, 100);
    }
    
    // Swap buffers if double buffering
    if (display->buffer_mode == ESPIO_DISPLAY_BUFFER_DOUBLE && display->buffer_count >= 2) {
        // Swap front/back
        uint8_t temp = display->front_buffer_index;
        display->front_buffer_index = display->back_buffer_index;
        display->back_buffer_index = temp;
    }
    
    void* fb = get_back_buffer(display);
    
    // Update driver's framebuffer pointer
    if (display->driver && display->driver->update_framebuffer) {
        return display->driver->update_framebuffer(display->driver_ctx, fb);
    }
    
    return ESPIO_DISPLAY_OK;
}

int32_t espio_display_stop_continuous(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;
    
    if (!display->continuous_active) {
        return ESPIO_DISPLAY_OK;
    }
    
    if (display->driver && display->driver->stop_continuous) {
        int32_t result = display->driver->stop_continuous(display->driver_ctx);
        if (result == ESPIO_DISPLAY_OK) {
            display->continuous_active = false;
            display->refresh_mode = ESPIO_DISPLAY_REFRESH_ON_DEMAND;
        }
        return result;
    }
    
    return ESPIO_DISPLAY_ERR_CONTINUOUS_NOT_SUPPORTED;
}

int32_t espio_display_wait_vsync(espio_display_t* display, uint32_t timeout_ms) {
    if (!display) return ESPIO_INVALID_ARG;
    
    if (display->driver && display->driver->wait_vsync) {
        return display->driver->wait_vsync(display->driver_ctx, timeout_ms);
    }
    
    return ESPIO_DISPLAY_OK;
}

/*============================================================================
 * Buffer Management Functions
 *============================================================================*/

bool espio_display_has_framebuffer(espio_display_t* display) {
    if (!display) return false;
    return has_framebuffer(display);
}

espio_display_buffer_mode_t espio_display_get_buffer_mode(espio_display_t* display) {
    if (!display) return ESPIO_DISPLAY_BUFFER_NONE;
    return display->buffer_mode;
}

bool espio_display_is_double_buffered(espio_display_t* display) {
    if (!display) return false;
    return display->buffer_mode == ESPIO_DISPLAY_BUFFER_DOUBLE && display->buffer_count >= 2;
}

int32_t espio_display_swap_buffers(espio_display_t* display) {
    if (!display) return ESPIO_INVALID_ARG;
    
    // Not in double buffer mode
    if (display->buffer_mode != ESPIO_DISPLAY_BUFFER_DOUBLE || display->buffer_count < 2) {
        ESPIO_LOGW(TAG, "espio_display_swap_buffers() called but not in double buffer mode");
        return ESPIO_DISPLAY_ERR_INVALID_BUFFER_MODE;
    }

    // Wait for previous flush to complete if still in progress
    if (display->flush_in_progress) {
        if (display->flush_complete) {
            // Non-blocking check - if still in progress, wait briefly
            if (!xSemaphoreTake(display->flush_complete, pdMS_TO_TICKS(100))) {
                ESPIO_LOGW(TAG, "Previous flush still in progress, waiting...");
                xSemaphoreTake(display->flush_complete, portMAX_DELAY);
            }
        }
        display->flush_in_progress = false;
    }

    // Swap buffer indices
    uint8_t temp = display->front_buffer_index;
    display->front_buffer_index = display->back_buffer_index;
    display->back_buffer_index = temp;

    // Start async flush of new front buffer
    if (display->driver && display->driver->flush) {
        display->flush_in_progress = true;
        void* front_fb = display->framebuffers[display->front_buffer_index];
        int32_t result = display->driver->flush(display->driver_ctx, front_fb,
                                             display->width, display->height);
        if (result != ESPIO_DISPLAY_OK) {
            ESPIO_LOGW(TAG, "Flush failed during buffer swap");
            display->flush_in_progress = false;
            return ESPIO_DISPLAY_ERR_DOUBLE_BUFFER_SWAP;
        }
        
        // For now, complete immediately (sync flush)
        // In Phase 3, this will be made async with DMA callback
        display->flush_in_progress = false;
        if (display->flush_complete) {
            xSemaphoreGive(display->flush_complete);
        }
    }

    return ESPIO_DISPLAY_OK;
}

int32_t espio_display_wait_for_swap(espio_display_t* display, int32_t timeout_ms) {
    if (!display) return ESPIO_INVALID_ARG;
    
    // Not in double buffer mode, nothing to wait for
    if (display->buffer_mode != ESPIO_DISPLAY_BUFFER_DOUBLE) {
        return ESPIO_DISPLAY_OK;
    }

    // No flush in progress
    if (!display->flush_in_progress) {
        return ESPIO_DISPLAY_OK;
    }

    // Wait for flush completion
    if (display->flush_complete) {
        TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        if (xSemaphoreTake(display->flush_complete, ticks) == pdTRUE) {
            display->flush_in_progress = false;
            return ESPIO_DISPLAY_OK;
        }
        return ESPIO_TIMEOUT;
    }
    
    return ESPIO_DISPLAY_OK;
}

/*============================================================================
 * Extended Feature Functions (Phase 5)
 *============================================================================*/

int32_t espio_display_get_caps(espio_display_t* display, espio_display_caps_t* caps) {
    if (!display || !caps) return ESPIO_INVALID_ARG;
    
    // Initialize with defaults
    memset(caps, 0, sizeof(espio_display_caps_t));
    caps->display_type = ESPIO_DISPLAY_TYPE_UNKNOWN;
    caps->supported_formats = (1 << display->format);
    
    // Set feature flags based on driver
    if (display->driver) {
        caps->has_backlight = display->driver->set_backlight != NULL;
        caps->has_partial_update = display->driver->flush_partial != NULL;
        caps->has_continuous_refresh = display->driver->start_continuous != NULL;
        caps->has_async_operations = display->driver->flush_async != NULL;
        caps->has_runtime_rotation = display->driver->set_rotation != NULL;
        caps->has_vsync = display->driver->wait_vsync != NULL;
        caps->has_read_pixel = has_framebuffer(display);  // Can read from framebuffer
    }
    
    // Get driver-specific capabilities
    if (display->driver && display->driver->get_caps) {
        return display->driver->get_caps(display->driver_ctx, caps);
    }
    
    return ESPIO_DISPLAY_OK;
}

int32_t espio_display_set_rotation(espio_display_t* display, espio_display_rotation_t rotation) {
    if (!display) return ESPIO_INVALID_ARG;
    if (rotation > ESPIO_DISPLAY_ROTATION_270) return ESPIO_INVALID_ARG;
    
    if (!display->driver || !display->driver->set_rotation) {
        return ESPIO_DISPLAY_ERR_ROTATION_NOT_SUPPORTED;
    }
    
    int32_t result = display->driver->set_rotation(display->driver_ctx, rotation);
    
    if (result == ESPIO_DISPLAY_OK) {
        // Swap width/height for 90/270 degree rotations
        if (rotation == ESPIO_DISPLAY_ROTATION_90 || rotation == ESPIO_DISPLAY_ROTATION_270) {
            uint16_t temp = display->width;
            display->width = display->height;
            display->height = temp;
        }
    }
    
    return result;
}

espio_display_rotation_t espio_display_get_rotation(espio_display_t* display) {
    if (!display) return ESPIO_DISPLAY_ROTATION_0;
    
    if (display->driver && display->driver->get_rotation) {
        return display->driver->get_rotation(display->driver_ctx);
    }
    
    return ESPIO_DISPLAY_ROTATION_0;
}

int32_t espio_display_set_power_state(espio_display_t* display, espio_display_power_state_t state) {
    if (!display) return ESPIO_INVALID_ARG;
    
    // Map extended states to basic on/off if driver doesn't support extended
    if (!display->driver || !display->driver->set_power_state) {
        if (display->driver && display->driver->set_power) {
            // Fall back to basic power control
            bool on = (state == ESPIO_DISPLAY_POWER_ON || state == ESPIO_DISPLAY_POWER_STANDBY);
            return display->driver->set_power(display->driver_ctx, on);
        }
        return ESPIO_DISPLAY_ERR_POWER_STATE_NOT_SUPPORTED;
    }
    
    return display->driver->set_power_state(display->driver_ctx, state);
}

espio_display_power_state_t espio_display_get_power_state(espio_display_t* display) {
    if (!display) return ESPIO_DISPLAY_POWER_OFF;
    
    if (display->driver && display->driver->get_power_state) {
        return display->driver->get_power_state(display->driver_ctx);
    }
    
    return ESPIO_DISPLAY_POWER_OFF;
}

int32_t espio_display_send_command(espio_display_t* display, uint32_t command,
                                   const void* params, size_t param_len,
                                   void* response, size_t* response_len) {
    if (!display) return ESPIO_INVALID_ARG;
    
    if (!display->driver || !display->driver->send_command) {
        return ESPIO_DISPLAY_ERR_COMMAND_NOT_SUPPORTED;
    }
    
    return display->driver->send_command(display->driver_ctx, command,
                                          params, param_len,
                                          response, response_len);
}

/**
 * @brief Return a stable display error name for logs and diagnostics.
 */
const char* espio_display_err_name(int32_t error) {
    const char* common_name = espio_err_common_name(error);
    if (common_name && strcmp(common_name, "UNKNOWN") != 0) {
        return common_name;
    }

    switch (error) {
        case ESPIO_DISPLAY_OK:
            return "ESPIO_DISPLAY_OK";
        case ESPIO_DISPLAY_ERR_DRIVER_FAILED:
            return "ESPIO_DISPLAY_ERR_DRIVER_FAILED";
        case ESPIO_DISPLAY_ERR_SPI_INIT:
            return "ESPIO_DISPLAY_ERR_SPI_INIT";
        case ESPIO_DISPLAY_ERR_SPI_WRITE:
            return "ESPIO_DISPLAY_ERR_SPI_WRITE";
        case ESPIO_DISPLAY_ERR_PANEL_INIT:
            return "ESPIO_DISPLAY_ERR_PANEL_INIT";
        case ESPIO_DISPLAY_ERR_PANEL_RESET:
            return "ESPIO_DISPLAY_ERR_PANEL_RESET";
        case ESPIO_DISPLAY_ERR_BACKLIGHT:
            return "ESPIO_DISPLAY_ERR_BACKLIGHT";
        case ESPIO_DISPLAY_ERR_FRAMEBUFFER:
            return "ESPIO_DISPLAY_ERR_FRAMEBUFFER";
        case ESPIO_DISPLAY_ERR_DMA:
            return "ESPIO_DISPLAY_ERR_DMA";
        case ESPIO_DISPLAY_ERR_OUT_OF_BOUNDS:
            return "ESPIO_DISPLAY_ERR_OUT_OF_BOUNDS";
        case ESPIO_DISPLAY_ERR_FLUSH_IN_PROGRESS:
            return "ESPIO_DISPLAY_ERR_FLUSH_IN_PROGRESS";
        case ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER:
            return "ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER";
        case ESPIO_DISPLAY_ERR_INVALID_BUFFER_MODE:
            return "ESPIO_DISPLAY_ERR_INVALID_BUFFER_MODE";
        case ESPIO_DISPLAY_ERR_DOUBLE_BUFFER_SWAP:
            return "ESPIO_DISPLAY_ERR_DOUBLE_BUFFER_SWAP";
        case ESPIO_DISPLAY_ERR_DRIVER_NOT_SET:
            return "ESPIO_DISPLAY_ERR_DRIVER_NOT_SET";
        case ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT:
            return "ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT";
        case ESPIO_DISPLAY_ERR_FORMAT_MISMATCH:
            return "ESPIO_DISPLAY_ERR_FORMAT_MISMATCH";
        case ESPIO_DISPLAY_ERR_CONVERSION_FAILED:
            return "ESPIO_DISPLAY_ERR_CONVERSION_FAILED";
        case ESPIO_DISPLAY_ERR_PARTIAL_NOT_SUPPORTED:
            return "ESPIO_DISPLAY_ERR_PARTIAL_NOT_SUPPORTED";
        case ESPIO_DISPLAY_ERR_PARTIAL_INVALID_REGION:
            return "ESPIO_DISPLAY_ERR_PARTIAL_INVALID_REGION";
        case ESPIO_DISPLAY_ERR_ASYNC_BUSY:
            return "ESPIO_DISPLAY_ERR_ASYNC_BUSY";
        case ESPIO_DISPLAY_ERR_ASYNC_TIMEOUT:
            return "ESPIO_DISPLAY_ERR_ASYNC_TIMEOUT";
        case ESPIO_DISPLAY_ERR_ASYNC_CANCELLED:
            return "ESPIO_DISPLAY_ERR_ASYNC_CANCELLED";
        case ESPIO_DISPLAY_ERR_CONTINUOUS_NOT_SUPPORTED:
            return "ESPIO_DISPLAY_ERR_CONTINUOUS_NOT_SUPPORTED";
        case ESPIO_DISPLAY_ERR_NOT_IN_CONTINUOUS_MODE:
            return "ESPIO_DISPLAY_ERR_NOT_IN_CONTINUOUS_MODE";
        case ESPIO_DISPLAY_ERR_VSYNC_TIMEOUT:
            return "ESPIO_DISPLAY_ERR_VSYNC_TIMEOUT";
        case ESPIO_DISPLAY_ERR_REFRESH_RATE:
            return "ESPIO_DISPLAY_ERR_REFRESH_RATE";
        case ESPIO_DISPLAY_ERR_ROTATION_NOT_SUPPORTED:
            return "ESPIO_DISPLAY_ERR_ROTATION_NOT_SUPPORTED";
        case ESPIO_DISPLAY_ERR_POWER_STATE_NOT_SUPPORTED:
            return "ESPIO_DISPLAY_ERR_POWER_STATE_NOT_SUPPORTED";
        case ESPIO_DISPLAY_ERR_COMMAND_NOT_SUPPORTED:
            return "ESPIO_DISPLAY_ERR_COMMAND_NOT_SUPPORTED";
        case ESPIO_DISPLAY_ERR_COMMAND_FAILED:
            return "ESPIO_DISPLAY_ERR_COMMAND_FAILED";
        case ESPIO_DISPLAY_ERR_READ_NOT_SUPPORTED:
            return "ESPIO_DISPLAY_ERR_READ_NOT_SUPPORTED";
        case ESPIO_DISPLAY_ERR_DRIVER_BASE:
            return "ESPIO_DISPLAY_ERR_DRIVER_BASE";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Return a short display error description for user-facing diagnostics.
 */
const char* espio_display_err_desc(int32_t error) {
    const char* common_desc = espio_err_common_desc(error);
    if (common_desc) {
        return common_desc;
    }

    switch (error) {
        case ESPIO_DISPLAY_OK:
            return "Success";
        case ESPIO_DISPLAY_ERR_DRIVER_FAILED:
            return "Driver initialization failed";
        case ESPIO_DISPLAY_ERR_SPI_INIT:
            return "SPI initialization failed";
        case ESPIO_DISPLAY_ERR_SPI_WRITE:
            return "SPI write operation failed";
        case ESPIO_DISPLAY_ERR_PANEL_INIT:
            return "Panel initialization failed";
        case ESPIO_DISPLAY_ERR_PANEL_RESET:
            return "Panel reset failed";
        case ESPIO_DISPLAY_ERR_BACKLIGHT:
            return "Backlight control failed";
        case ESPIO_DISPLAY_ERR_FRAMEBUFFER:
            return "Framebuffer allocation failed";
        case ESPIO_DISPLAY_ERR_DMA:
            return "DMA transfer failed";
        case ESPIO_DISPLAY_ERR_OUT_OF_BOUNDS:
            return "Coordinates out of bounds";
        case ESPIO_DISPLAY_ERR_FLUSH_IN_PROGRESS:
            return "Flush already in progress";
        case ESPIO_DISPLAY_ERR_NO_FRAMEBUFFER:
            return "No framebuffer allocated";
        case ESPIO_DISPLAY_ERR_INVALID_BUFFER_MODE:
            return "Invalid buffer mode";
        case ESPIO_DISPLAY_ERR_DOUBLE_BUFFER_SWAP:
            return "Double buffer swap failed";
        case ESPIO_DISPLAY_ERR_DRIVER_NOT_SET:
            return "Driver not set in configuration";
        case ESPIO_DISPLAY_ERR_UNSUPPORTED_FORMAT:
            return "Pixel format not supported";
        case ESPIO_DISPLAY_ERR_FORMAT_MISMATCH:
            return "Data format does not match display";
        case ESPIO_DISPLAY_ERR_CONVERSION_FAILED:
            return "Color conversion failed";
        case ESPIO_DISPLAY_ERR_PARTIAL_NOT_SUPPORTED:
            return "Partial update not supported";
        case ESPIO_DISPLAY_ERR_PARTIAL_INVALID_REGION:
            return "Invalid partial update region";
        case ESPIO_DISPLAY_ERR_ASYNC_BUSY:
            return "Async operation already in progress";
        case ESPIO_DISPLAY_ERR_ASYNC_TIMEOUT:
            return "Async operation timed out";
        case ESPIO_DISPLAY_ERR_ASYNC_CANCELLED:
            return "Async operation was cancelled";
        case ESPIO_DISPLAY_ERR_CONTINUOUS_NOT_SUPPORTED:
            return "Continuous refresh not supported";
        case ESPIO_DISPLAY_ERR_NOT_IN_CONTINUOUS_MODE:
            return "Not in continuous refresh mode";
        case ESPIO_DISPLAY_ERR_VSYNC_TIMEOUT:
            return "Vsync wait timed out";
        case ESPIO_DISPLAY_ERR_REFRESH_RATE:
            return "Invalid refresh rate";
        case ESPIO_DISPLAY_ERR_ROTATION_NOT_SUPPORTED:
            return "Rotation not supported";
        case ESPIO_DISPLAY_ERR_POWER_STATE_NOT_SUPPORTED:
            return "Power state not supported";
        case ESPIO_DISPLAY_ERR_COMMAND_NOT_SUPPORTED:
            return "Command not supported";
        case ESPIO_DISPLAY_ERR_COMMAND_FAILED:
            return "Command execution failed";
        case ESPIO_DISPLAY_ERR_READ_NOT_SUPPORTED:
            return "Pixel read not supported";
        default:
            return NULL;
    }
}
