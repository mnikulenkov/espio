/**
 * @file sd.c
 * @brief SD card abstraction facade for ESP-IDF v5 storage backends.
 */

#include "espio/sd.h"
#include "espio/err_log.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "espio/log.h"
#include "sdmmc_cmd.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "sd";

/**
 * @brief Internal file node tracked by the card facade.
 */
struct espio_sd_file {
    espio_sd_t* card;                 ///< Owning SD card handle.
    FILE* stream;                     ///< Underlying C stdio stream.
    SemaphoreHandle_t lock;           ///< Per-file lock for read/write/close serialization.
    struct espio_sd_file* next;       ///< Intrusive linked-list next pointer.
    struct espio_sd_file* prev;       ///< Intrusive linked-list previous pointer.
    bool closed;                      ///< True after the file handle has been closed.
};

/**
 * @brief Internal SD card state.
 */
struct espio_sd {
    espio_sd_config_t config;         ///< Effective configuration copied at creation time.
    char* mount_point;                ///< Normalized mount point without a trailing slash.
    SemaphoreHandle_t lock;           ///< Card-level lock for mount state and file registry updates.
    sdmmc_card_t* card;               ///< ESP-IDF card descriptor returned by the mount helper.
    espio_sd_file_t* files_head;      ///< Head of the open-file list.
    size_t open_file_count;           ///< Number of open file handles tracked by the facade.
    bool mounted;                     ///< True once the filesystem is mounted.
    bool unmount_requested;           ///< True while unmount is waiting for open files to drain.
    bool power_gpio_configured;       ///< True once the optional power GPIO has been configured.
};

/**
 * @brief Validate an optional pin field that may use ESPIO_SD_PIN_UNUSED.
 */
static bool sd_pin_is_optional_valid(int pin) {
    return pin == ESPIO_SD_PIN_UNUSED || pin >= 0;
}

/**
 * @brief Map ESP-IDF errors to SD error codes for common cases.
 *
 * Used internally to convert esp_err_t to appropriate SD error codes.
 * The native error is preserved via err_set_last().
 */
static int32_t sd_map_esp_error(esp_err_t error) {
    switch (error) {
        case ESP_OK:
            return ESPIO_SD_OK;
        case ESP_ERR_INVALID_ARG:
            return ESPIO_INVALID_ARG;
        case ESP_ERR_INVALID_STATE:
            return ESPIO_BUSY;
        case ESP_ERR_NO_MEM:
            return ESPIO_NO_MEM;
        case ESP_ERR_TIMEOUT:
            return ESPIO_TIMEOUT;
        case ESP_ERR_NOT_FOUND:
            return ESPIO_NOT_FOUND;
        case ESP_FAIL:
            return ESPIO_SD_ERR_MOUNT_FAILED;
        default:
            return ESPIO_IO;
    }
}

/**
 * @brief Map libc errno values to SD error codes.
 *
 * Used internally to convert errno to appropriate SD error codes.
 * The native error is preserved via err_set_last().
 */
static int32_t sd_map_errno(int error) {
    switch (error) {
        case 0:
            return ESPIO_SD_OK;
        case ENOENT:
            return ESPIO_NOT_FOUND;
        case EEXIST:
            return ESPIO_SD_ERR_ALREADY_EXISTS;
        case ENOSPC:
            return ESPIO_SD_ERR_NO_SPACE;
        case EMFILE:
        case ENFILE:
            return ESPIO_SD_ERR_TOO_MANY_OPEN_FILES;
        case EBUSY:
            return ESPIO_BUSY;
        case ETIMEDOUT:
            return ESPIO_TIMEOUT;
        case ENOMEM:
            return ESPIO_NO_MEM;
        case EINVAL:
        case ENAMETOOLONG:
            return ESPIO_SD_ERR_PATH_INVALID;
        default:
            return ESPIO_IO;
    }
}

/**
 * @brief Resolve the default max-open-file policy when the caller leaves it unset.
 */
static size_t sd_effective_max_open_files(const espio_sd_config_t* config) {
    if (!config || config->mount.max_open_files == 0U) {
        return ESPIO_SD_MAX_OPEN_FILES_DEFAULT;
    }
    return config->mount.max_open_files;
}

/**
 * @brief Normalize a mount point to one leading slash and no trailing slash.
 *
 * @param mount_point Input mount point string
 * @return Normalized string (caller must free), or NULL on error (sets thread-local error)
 */
static char* sd_normalize_mount_point(const char* mount_point) {
    const char* source = mount_point;
    if (!source || source[0] == '\0') {
        source = ESPIO_SD_MOUNT_POINT_DEFAULT;
    }

    while (*source == '/') {
        source++;
    }

    if (*source == '\0') {
        ESPIO_SD_SET_ERROR(ESPIO_SD_ERR_PATH_INVALID, 0, "empty mount point");
        return NULL;
    }

    size_t source_len = strlen(source);
    while (source_len > 0U && source[source_len - 1U] == '/') {
        source_len--;
    }

    if (source_len == 0U) {
        ESPIO_SD_SET_ERROR(ESPIO_SD_ERR_PATH_INVALID, 0, "mount point is all slashes");
        return NULL;
    }

    char* normalized = calloc(source_len + 2U, sizeof(char));
    if (!normalized) {
        ESPIO_SD_SET_ERROR(ESPIO_NO_MEM, 0, "failed to allocate mount point");
        return NULL;
    }

    normalized[0] = '/';
    memcpy(normalized + 1U, source, source_len);

    return normalized;
}

/**
 * @brief Resolve a logical path relative to the stored mount point.
 *
 * @param card SD card handle
 * @param logical_path Relative path to resolve
 * @return Resolved path (caller must free), or NULL on error (sets thread-local error)
 */
static char* sd_resolve_path(const espio_sd_t* card, const char* logical_path) {
    if (!card || !card->mount_point || !logical_path) {
        ESPIO_SD_SET_ERROR(ESPIO_INVALID_ARG, 0, "null argument");
        return NULL;
    }

    const char* relative = logical_path;
    while (*relative == '/') {
        relative++;
    }

    if (*relative == '\0') {
        ESPIO_SD_SET_ERROR(ESPIO_SD_ERR_PATH_INVALID, 0, "empty logical path");
        return NULL;
    }

    size_t mount_len = strlen(card->mount_point);
    size_t relative_len = strlen(relative);
    size_t total_len = mount_len + 1U + relative_len + 1U;
    if (total_len < mount_len || total_len < relative_len) {
        ESPIO_SD_SET_ERROR(ESPIO_SD_ERR_PATH_TOO_LONG, 0, "path length overflow");
        return NULL;
    }

    char* path = calloc(total_len, sizeof(char));
    if (!path) {
        ESPIO_SD_SET_ERROR(ESPIO_NO_MEM, 0, "failed to allocate path");
        return NULL;
    }

    memcpy(path, card->mount_point, mount_len);
    path[mount_len] = '/';
    memcpy(path + mount_len + 1U, relative, relative_len);

    return path;
}

/**
 * @brief Convert the public open mode into a stdio mode string.
 */
static const char* sd_open_mode_to_string(espio_sd_open_mode_t mode) {
    switch (mode) {
        case ESPIO_SD_OPEN_MODE_READ:
            return "rb";
        case ESPIO_SD_OPEN_MODE_WRITE_TRUNCATE:
            return "wb";
        case ESPIO_SD_OPEN_MODE_WRITE_APPEND:
            return "ab";
        case ESPIO_SD_OPEN_MODE_READWRITE:
            return "rb+";
        case ESPIO_SD_OPEN_MODE_READWRITE_CREATE:
            return "rb+";
        default:
            return NULL;
    }
}

/**
 * @brief Tell whether a mode should retry by creating a missing file.
 */
static bool sd_open_mode_allows_create_retry(espio_sd_open_mode_t mode) {
    return mode == ESPIO_SD_OPEN_MODE_READWRITE_CREATE;
}

/**
 * @brief Translate the public seek origin into the libc constant.
 */
static int sd_seek_origin_to_libc(espio_sd_seek_origin_t origin) {
    switch (origin) {
        case ESPIO_SD_SEEK_SET:
            return SEEK_SET;
        case ESPIO_SD_SEEK_CUR:
            return SEEK_CUR;
        case ESPIO_SD_SEEK_END:
            return SEEK_END;
        default:
            return -1;
    }
}

/**
 * @brief Apply board-level power sequencing once the control GPIO is configured.
 */
static int32_t sd_set_power_state(espio_sd_t* card, bool enabled) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    if (card->config.power.power_gpio == ESPIO_SD_PIN_UNUSED) {
        ESPIO_SD_RETURN_OK();
    }

    int active_level = card->config.power.power_active_high ? 1 : 0;
    int level = enabled ? active_level : !active_level;
    esp_err_t esp_err = gpio_set_level((gpio_num_t)card->config.power.power_gpio, level);
    if (esp_err != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to drive power GPIO: %s", esp_err_to_name(esp_err));
        ESPIO_SD_RETURN(ESPIO_SD_ERR_POWER_FAILED, esp_err, "failed to drive power GPIO");
    }

    uint32_t delay_ms = enabled ? card->config.power.power_on_delay_ms : card->config.power.power_off_delay_ms;
    if (delay_ms > 0U) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Configure the optional power GPIO once during handle creation.
 */
static int32_t sd_prepare_power_gpio(espio_sd_t* card) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    if (card->config.power.power_gpio == ESPIO_SD_PIN_UNUSED) {
        ESPIO_SD_RETURN_OK();
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << card->config.power.power_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t error = gpio_config(&gpio_cfg);
    if (error != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to configure power GPIO: %s", esp_err_to_name(error));
        ESPIO_SD_RETURN(ESPIO_SD_ERR_POWER_FAILED, error, "failed to configure power GPIO");
    }

    card->power_gpio_configured = true;
    return sd_set_power_state(card, false);
}

/**
 * @brief Release the optional power GPIO during final destruction.
 */
static void sd_release_power_gpio(espio_sd_t* card) {
    if (!card || !card->power_gpio_configured || card->config.power.power_gpio == ESPIO_SD_PIN_UNUSED) {
        return;
    }

    (void)gpio_reset_pin((gpio_num_t)card->config.power.power_gpio);
    card->power_gpio_configured = false;
}

/**
 * @brief Apply host-side debug pull-ups for SPI-based card connections.
 */
static int32_t sd_enable_spi_pullups(const espio_sd_spi_config_t* config) {
    if (!config || !config->enable_internal_pullups) {
        ESPIO_SD_RETURN_OK();
    }

    const int pins[] = {
        config->mosi_pin,
        config->miso_pin,
        config->cs_pin,
    };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        esp_err_t error = gpio_set_pull_mode((gpio_num_t)pins[i], GPIO_PULLUP_ONLY);
        if (error != ESP_OK) {
            ESPIO_LOGE(TAG, "failed to enable SPI pull-up on GPIO %d: %s", pins[i], esp_err_to_name(error));
            ESPIO_SD_RETURN(ESPIO_SD_ERR_HOST_INIT_FAILED, error, "failed to enable SPI pull-up");
        }
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Initialize an owned SPI bus when the facade is responsible for it.
 */
static int32_t sd_prepare_spi_bus(espio_sd_t* card) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    const espio_sd_spi_config_t* config = &card->config.bus.spi;
    if (!config->bus_managed_by_sd) {
        ESPIO_SD_RETURN_OK();
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = config->mosi_pin,
        .miso_io_num = config->miso_pin,
        .sclk_io_num = config->sck_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    esp_err_t error = spi_bus_initialize((spi_host_device_t)config->host_id, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to initialize owned SPI bus: %s", esp_err_to_name(error));
        ESPIO_SD_RETURN(ESPIO_SD_ERR_HOST_INIT_FAILED, error, "failed to initialize owned SPI bus");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Release an owned SPI bus after the card has been unmounted.
 */
static void sd_release_spi_bus(espio_sd_t* card) {
    if (!card || card->config.mode != ESPIO_SD_BUS_MODE_SPI || !card->config.bus.spi.bus_managed_by_sd) {
        return;
    }

    esp_err_t error = spi_bus_free((spi_host_device_t)card->config.bus.spi.host_id);
    if (error != ESP_OK) {
        ESPIO_LOGW(TAG, "failed to free owned SPI bus: %s", esp_err_to_name(error));
    }
}

/**
 * @brief Execute the SDMMC-specific mount flow.
 */
static int32_t sd_mount_sdmmc(espio_sd_t* card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = card->config.bus.sdmmc.slot;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = card->config.bus.sdmmc.width;
    slot.cd = card->config.bus.sdmmc.cd_pin == ESPIO_SD_PIN_UNUSED ? SDMMC_SLOT_NO_CD : (gpio_num_t)card->config.bus.sdmmc.cd_pin;
    slot.wp = card->config.bus.sdmmc.wp_pin == ESPIO_SD_PIN_UNUSED ? SDMMC_SLOT_NO_WP : (gpio_num_t)card->config.bus.sdmmc.wp_pin;
    if (card->config.bus.sdmmc.enable_internal_pullups) {
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = card->config.mount.format_if_mount_failed,
        .max_files = (int)sd_effective_max_open_files(&card->config),
        .allocation_unit_size = card->config.mount.allocation_unit_size,
        .disk_status_check_enable = card->config.mount.disk_status_check,
    };

    esp_err_t error = esp_vfs_fat_sdmmc_mount(card->mount_point, &host, &slot, &mount_config, &card->card);
    if (error != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to mount SDMMC card: %s", esp_err_to_name(error));
        int32_t translated = sd_map_esp_error(error);
        if (translated == ESPIO_SD_ERR_MOUNT_FAILED) {
            translated = ESPIO_SD_ERR_CARD_INIT_FAILED;
        }
        ESPIO_SD_RETURN(translated, error, "failed to mount SDMMC card");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Execute the SPI-specific mount flow.
 */
static int32_t sd_mount_spi(espio_sd_t* card) {
    int32_t error = sd_enable_spi_pullups(&card->config.bus.spi);
    if (error != ESPIO_SD_OK) {
        return error;
    }

    error = sd_prepare_spi_bus(card);
    if (error != ESPIO_SD_OK) {
        return error;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = card->config.bus.spi.host_id;
    host.max_freq_khz = (int)card->config.bus.spi.max_freq_khz;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = (spi_host_device_t)card->config.bus.spi.host_id;
    slot.gpio_cs = (gpio_num_t)card->config.bus.spi.cs_pin;
    slot.gpio_cd = card->config.bus.spi.cd_pin == ESPIO_SD_PIN_UNUSED ? SDSPI_SLOT_NO_CD : (gpio_num_t)card->config.bus.spi.cd_pin;
    slot.gpio_wp = card->config.bus.spi.wp_pin == ESPIO_SD_PIN_UNUSED ? SDSPI_SLOT_NO_WP : (gpio_num_t)card->config.bus.spi.wp_pin;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = card->config.mount.format_if_mount_failed,
        .max_files = (int)sd_effective_max_open_files(&card->config),
        .allocation_unit_size = card->config.mount.allocation_unit_size,
        .disk_status_check_enable = card->config.mount.disk_status_check,
    };

    esp_err_t esp_error = esp_vfs_fat_sdspi_mount(card->mount_point, &host, &slot, &mount_config, &card->card);
    if (esp_error != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to mount SDSPI card: %s", esp_err_to_name(esp_error));
        sd_release_spi_bus(card);
        int32_t translated = sd_map_esp_error(esp_error);
        if (translated == ESPIO_SD_ERR_MOUNT_FAILED) {
            translated = ESPIO_SD_ERR_CARD_INIT_FAILED;
        }
        ESPIO_SD_RETURN(translated, esp_error, "failed to mount SDSPI card");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Validate the caller-facing configuration before allocating resources.
 */
static int32_t sd_validate_config(const espio_sd_config_t* config) {
    if (!config) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null config");
    }

    if (!sd_pin_is_optional_valid(config->power.power_gpio)) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid power GPIO");
    }

    switch (config->mode) {
        case ESPIO_SD_BUS_MODE_SDMMC:
            if ((config->bus.sdmmc.width != 0U) &&
                (config->bus.sdmmc.width != 1U) &&
                (config->bus.sdmmc.width != 4U)) {
                ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid SDMMC width");
            }

            if (!sd_pin_is_optional_valid(config->bus.sdmmc.cd_pin) ||
                !sd_pin_is_optional_valid(config->bus.sdmmc.wp_pin) ||
                config->bus.sdmmc.slot < 0) {
                ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid SDMMC pin config");
            }
            break;

        case ESPIO_SD_BUS_MODE_SPI:
            if (config->bus.spi.host_id < 0 ||
                config->bus.spi.sck_pin < 0 ||
                config->bus.spi.mosi_pin < 0 ||
                config->bus.spi.miso_pin < 0 ||
                config->bus.spi.cs_pin < 0 ||
                !sd_pin_is_optional_valid(config->bus.spi.cd_pin) ||
                !sd_pin_is_optional_valid(config->bus.spi.wp_pin)) {
                ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid SPI pin config");
            }
            break;

        default:
            ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid bus mode");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Copy the caller configuration and fill in stable defaults.
 */
static void sd_apply_config_defaults(espio_sd_config_t* dst, const espio_sd_config_t* src) {
    *dst = *src;

    if (dst->mount.max_open_files == 0U) {
        dst->mount.max_open_files = ESPIO_SD_MAX_OPEN_FILES_DEFAULT;
    }

    if (dst->mode == ESPIO_SD_BUS_MODE_SDMMC) {
        if (dst->bus.sdmmc.width == 0U) {
            dst->bus.sdmmc.width = 4U;
        }
    } else if (dst->mode == ESPIO_SD_BUS_MODE_SPI) {
        if (dst->bus.spi.max_freq_khz == 0U) {
            dst->bus.spi.max_freq_khz = 20000U;
        }
    }
}

/**
 * @brief Unlink a file node from the card registry once closing has finished.
 */
static void sd_detach_file_node(espio_sd_t* card, espio_sd_file_t* file) {
    if (!card || !file) {
        return;
    }

    if (file->prev) {
        file->prev->next = file->next;
    } else {
        card->files_head = file->next;
    }

    if (file->next) {
        file->next->prev = file->prev;
    }

    if (card->open_file_count > 0U) {
        card->open_file_count--;
    }
}

/**
 * @brief Mount the card while the caller already owns the card lock.
 */
static int32_t sd_mount_locked(espio_sd_t* card) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    if (card->mounted) {
        ESPIO_SD_RETURN(ESPIO_SD_ERR_ALREADY_MOUNTED, 0, "card already mounted");
    }

    int32_t error = sd_set_power_state(card, true);
    if (error != ESPIO_SD_OK) {
        return error;
    }

    switch (card->config.mode) {
        case ESPIO_SD_BUS_MODE_SDMMC:
            error = sd_mount_sdmmc(card);
            break;
        case ESPIO_SD_BUS_MODE_SPI:
            error = sd_mount_spi(card);
            break;
        default:
            ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid bus mode");
    }

    if (error != ESPIO_SD_OK) {
        (void)sd_set_power_state(card, false);
        return error;
    }

    card->mounted = true;
    card->unmount_requested = false;

    ESPIO_LOGI(TAG, "SD card mounted at %s", card->mount_point);
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Unmount the card while the caller already owns the card lock.
 */
static int32_t sd_unmount_locked(espio_sd_t* card) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    esp_err_t error = esp_vfs_fat_sdcard_unmount(card->mount_point, card->card);
    if (error != ESP_OK) {
        ESPIO_LOGE(TAG, "failed to unmount SD card: %s", esp_err_to_name(error));
        int32_t mapped_error = sd_map_esp_error(error);
        ESPIO_SD_RETURN(mapped_error, error, "failed to unmount SD card");
    }

    card->card = NULL;
    card->mounted = false;
    card->unmount_requested = false;

    if (card->config.mode == ESPIO_SD_BUS_MODE_SPI && card->config.bus.spi.bus_managed_by_sd) {
        sd_release_spi_bus(card);
    }

    int32_t power_error = sd_set_power_state(card, false);
    if (power_error != ESPIO_SD_OK) {
        return power_error;
    }

    ESPIO_LOGI(TAG, "SD card unmounted from %s", card->mount_point);
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Create, configure, and mount an SD card facade.
 */
int32_t espio_sd_create(espio_sd_t** out_card, const espio_sd_config_t* config) {
    if (!out_card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null out_card");
    }

    *out_card = NULL;

    int32_t validation_error = sd_validate_config(config);
    if (validation_error != ESPIO_SD_OK) {
        return validation_error;
    }

    espio_sd_t* card = calloc(1U, sizeof(espio_sd_t));
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_NO_MEM, 0, "failed to allocate card");
    }

    sd_apply_config_defaults(&card->config, config);

    card->mount_point = sd_normalize_mount_point(card->config.mount.mount_point);
    if (!card->mount_point) {
        free(card);
        return espio_err_get_last()->code;
    }

    card->lock = xSemaphoreCreateMutex();
    if (!card->lock) {
        free(card->mount_point);
        free(card);
        ESPIO_SD_RETURN(ESPIO_NO_MEM, 0, "failed to create mutex");
    }

    int32_t power_error = sd_prepare_power_gpio(card);
    if (power_error != ESPIO_SD_OK) {
        vSemaphoreDelete(card->lock);
        free(card->mount_point);
        free(card);
        return power_error;
    }

    xSemaphoreTake(card->lock, portMAX_DELAY);
    int32_t mount_error = sd_mount_locked(card);
    xSemaphoreGive(card->lock);
    if (mount_error != ESPIO_SD_OK) {
        sd_release_power_gpio(card);
        vSemaphoreDelete(card->lock);
        free(card->mount_point);
        free(card);
        return mount_error;
    }

    *out_card = card;
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Mount an existing SD card facade after sd_unmount().
 */
int32_t espio_sd_mount(espio_sd_t* card) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    xSemaphoreTake(card->lock, portMAX_DELAY);
    int32_t error = sd_mount_locked(card);
    xSemaphoreGive(card->lock);
    return error;
}

/**
 * @brief Unmount an SD card and release the active filesystem registration.
 */
int32_t espio_sd_unmount(espio_sd_t* card, uint32_t timeout_ms) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    xSemaphoreTake(card->lock, portMAX_DELAY);
    if (!card->mounted) {
        xSemaphoreGive(card->lock);
        ESPIO_SD_RETURN(ESPIO_SD_ERR_NOT_MOUNTED, 0, "card not mounted");
    }
    card->unmount_requested = true;
    xSemaphoreGive(card->lock);

    for (;;) {
        xSemaphoreTake(card->lock, portMAX_DELAY);
        if (card->open_file_count == 0U) {
            int32_t error = sd_unmount_locked(card);
            xSemaphoreGive(card->lock);
            return error;
        }
        xSemaphoreGive(card->lock);

        if (timeout_ticks == 0U) {
            xSemaphoreTake(card->lock, portMAX_DELAY);
            card->unmount_requested = false;
            xSemaphoreGive(card->lock);
            ESPIO_SD_RETURN(ESPIO_BUSY, 0, "files still open, no timeout");
        }

        if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            xSemaphoreTake(card->lock, portMAX_DELAY);
            card->unmount_requested = false;
            xSemaphoreGive(card->lock);
            ESPIO_SD_RETURN(ESPIO_TIMEOUT, 0, "unmount timed out");
        }

        vTaskDelay(1);
    }
}

/**
 * @brief Destroy an SD card facade after ensuring it is unmounted.
 */
int32_t espio_sd_destroy(espio_sd_t* card) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    if (card->mounted) {
        int32_t unmount_error = espio_sd_unmount(card, 0U);
        if (unmount_error != ESPIO_SD_OK) {
            return unmount_error;
        }
    }

    if (card->open_file_count != 0U) {
        ESPIO_SD_RETURN(ESPIO_BUSY, 0, "files still open");
    }

    sd_release_power_gpio(card);
    vSemaphoreDelete(card->lock);
    free(card->mount_point);
    free(card);

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Check whether the card is currently mounted.
 */
bool espio_sd_is_mounted(const espio_sd_t* card) {
    if (!card) {
        return false;
    }
    return card->mounted;
}

/**
 * @brief Get the normalized mount point used by the facade.
 */
const char* espio_sd_get_mount_point(const espio_sd_t* card) {
    if (!card) {
        return NULL;
    }
    return card->mount_point;
}

/**
 * @brief Convert a public error code into a stable string.
 */
const char* espio_sd_err_name(int32_t error) {
    // Check common errors first
    const char* common_name = espio_err_common_name(error);
    if (common_name && common_name[0] != '\0') {
        return common_name;
    }
    
    // SD-specific errors
    switch (error) {
        case ESPIO_SD_OK:
            return "ESPIO_SD_OK";
        case ESPIO_SD_ERR_ALREADY_MOUNTED:
            return "ESPIO_SD_ERR_ALREADY_MOUNTED";
        case ESPIO_SD_ERR_NOT_MOUNTED:
            return "ESPIO_SD_ERR_NOT_MOUNTED";
        case ESPIO_SD_ERR_ALREADY_EXISTS:
            return "ESPIO_SD_ERR_ALREADY_EXISTS";
        case ESPIO_SD_ERR_NO_SPACE:
            return "ESPIO_SD_ERR_NO_SPACE";
        case ESPIO_SD_ERR_TOO_MANY_OPEN_FILES:
            return "ESPIO_SD_ERR_TOO_MANY_OPEN_FILES";
        case ESPIO_SD_ERR_MOUNT_FAILED:
            return "ESPIO_SD_ERR_MOUNT_FAILED";
        case ESPIO_SD_ERR_UNMOUNT_FAILED:
            return "ESPIO_SD_ERR_UNMOUNT_FAILED";
        case ESPIO_SD_ERR_CARD_INIT_FAILED:
            return "ESPIO_SD_ERR_CARD_INIT_FAILED";
        case ESPIO_SD_ERR_HOST_INIT_FAILED:
            return "ESPIO_SD_ERR_HOST_INIT_FAILED";
        case ESPIO_SD_ERR_PATH_INVALID:
            return "ESPIO_SD_ERR_PATH_INVALID";
        case ESPIO_SD_ERR_PATH_TOO_LONG:
            return "ESPIO_SD_ERR_PATH_TOO_LONG";
        case ESPIO_SD_ERR_POWER_FAILED:
            return "ESPIO_SD_ERR_POWER_FAILED";
        case ESPIO_SD_ERR_FILE_NOT_OPEN:
            return "ESPIO_SD_ERR_FILE_NOT_OPEN";
        case ESPIO_SD_ERR_FILE_CLOSED:
            return "ESPIO_SD_ERR_FILE_CLOSED";
        case ESPIO_SD_ERR_READ_FAILED:
            return "ESPIO_SD_ERR_READ_FAILED";
        case ESPIO_SD_ERR_WRITE_FAILED:
            return "ESPIO_SD_ERR_WRITE_FAILED";
        case ESPIO_SD_ERR_SEEK_FAILED:
            return "ESPIO_SD_ERR_SEEK_FAILED";
        case ESPIO_SD_ERR_FLUSH_FAILED:
            return "ESPIO_SD_ERR_FLUSH_FAILED";
        case ESPIO_SD_ERR_STAT_FAILED:
            return "ESPIO_SD_ERR_STAT_FAILED";
        case ESPIO_SD_ERR_DELETE_FAILED:
            return "ESPIO_SD_ERR_DELETE_FAILED";
        case ESPIO_SD_ERR_MKDIR_FAILED:
            return "ESPIO_SD_ERR_MKDIR_FAILED";
        case ESPIO_SD_ERR_RMDIR_FAILED:
            return "ESPIO_SD_ERR_RMDIR_FAILED";
        case ESPIO_SD_ERR_OPENDIR_FAILED:
            return "ESPIO_SD_ERR_OPENDIR_FAILED";
        case ESPIO_SD_ERR_NOT_A_DIRECTORY:
            return "ESPIO_SD_ERR_NOT_A_DIRECTORY";
        case ESPIO_SD_ERR_UNMOUNT_PENDING:
            return "ESPIO_SD_ERR_UNMOUNT_PENDING";
        default:
            return "ESPIO_SD_ERR_UNKNOWN";
    }
}

/**
 * @brief Convert a public error code into a stable description string.
 */
const char* espio_sd_err_desc(int32_t error) {
    switch (error) {
        case ESPIO_SD_OK:
            return "operation completed successfully";
        case ESPIO_SD_ERR_ALREADY_MOUNTED:
            return "card is already mounted";
        case ESPIO_SD_ERR_NOT_MOUNTED:
            return "card is not mounted";
        case ESPIO_SD_ERR_ALREADY_EXISTS:
            return "path already exists";
        case ESPIO_SD_ERR_NO_SPACE:
            return "storage is full";
        case ESPIO_SD_ERR_TOO_MANY_OPEN_FILES:
            return "too many files are open";
        case ESPIO_SD_ERR_MOUNT_FAILED:
            return "mount operation failed";
        case ESPIO_SD_ERR_UNMOUNT_FAILED:
            return "unmount operation failed";
        case ESPIO_SD_ERR_CARD_INIT_FAILED:
            return "card initialization failed";
        case ESPIO_SD_ERR_HOST_INIT_FAILED:
            return "host initialization failed";
        case ESPIO_SD_ERR_PATH_INVALID:
            return "path is invalid";
        case ESPIO_SD_ERR_PATH_TOO_LONG:
            return "path is too long";
        case ESPIO_SD_ERR_POWER_FAILED:
            return "power control failed";
        case ESPIO_SD_ERR_FILE_NOT_OPEN:
            return "file is not open";
        case ESPIO_SD_ERR_FILE_CLOSED:
            return "file is already closed";
        case ESPIO_SD_ERR_READ_FAILED:
            return "read operation failed";
        case ESPIO_SD_ERR_WRITE_FAILED:
            return "write operation failed";
        case ESPIO_SD_ERR_SEEK_FAILED:
            return "seek operation failed";
        case ESPIO_SD_ERR_FLUSH_FAILED:
            return "flush operation failed";
        case ESPIO_SD_ERR_STAT_FAILED:
            return "stat operation failed";
        case ESPIO_SD_ERR_DELETE_FAILED:
            return "delete operation failed";
        case ESPIO_SD_ERR_MKDIR_FAILED:
            return "directory creation failed";
        case ESPIO_SD_ERR_RMDIR_FAILED:
            return "directory removal failed";
        case ESPIO_SD_ERR_OPENDIR_FAILED:
            return "directory open failed";
        case ESPIO_SD_ERR_NOT_A_DIRECTORY:
            return "path is not a directory";
        case ESPIO_SD_ERR_UNMOUNT_PENDING:
            return "unmount is pending";
        default:
            return espio_err_common_desc(error);
    }
}

/**
 * @brief Open a file relative to the configured mount point.
 */
int32_t espio_sd_file_open(espio_sd_t* card, const char* logical_path, espio_sd_open_mode_t mode, espio_sd_file_t** out_file) {
    if (!card || !out_file) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card or out_file");
    }

    *out_file = NULL;

    const char* fopen_mode = sd_open_mode_to_string(mode);
    if (!fopen_mode) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid open mode");
    }

    char* full_path = sd_resolve_path(card, logical_path);
    if (!full_path) {
        return espio_err_get_last()->code;
    }

    espio_sd_file_t* file = calloc(1U, sizeof(espio_sd_file_t));
    if (!file) {
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_NO_MEM, 0, "failed to allocate file handle");
    }

    file->lock = xSemaphoreCreateMutex();
    if (!file->lock) {
        free(file);
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_NO_MEM, 0, "failed to create file mutex");
    }

    xSemaphoreTake(card->lock, portMAX_DELAY);
    if (!card->mounted || card->unmount_requested) {
        xSemaphoreGive(card->lock);
        vSemaphoreDelete(file->lock);
        free(file);
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_SD_ERR_NOT_MOUNTED, 0, "card not mounted");
    }

    if (card->open_file_count >= sd_effective_max_open_files(&card->config)) {
        xSemaphoreGive(card->lock);
        vSemaphoreDelete(file->lock);
        free(file);
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_SD_ERR_TOO_MANY_OPEN_FILES, 0, "too many open files");
    }

    errno = 0;
    file->stream = fopen(full_path, fopen_mode);
    if (!file->stream && errno == ENOENT && sd_open_mode_allows_create_retry(mode)) {
        errno = 0;
        file->stream = fopen(full_path, "wb+");
    }
    if (!file->stream) {
        int open_errno = errno;
        xSemaphoreGive(card->lock);
        vSemaphoreDelete(file->lock);
        free(file);
        free(full_path);
        ESPIO_SD_RETURN(sd_map_errno(open_errno), open_errno, "failed to open file");
    }

    file->card = card;
    file->next = card->files_head;
    if (card->files_head) {
        card->files_head->prev = file;
    }
    card->files_head = file;
    card->open_file_count++;
    xSemaphoreGive(card->lock);

    free(full_path);
    *out_file = file;
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Close a file handle and flush any pending buffered writes.
 */
int32_t espio_sd_file_close(espio_sd_file_t* file) {
    if (!file) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file already closed");
    }

    errno = 0;
    int close_result = fclose(file->stream);
    int close_errno = errno;
    file->stream = NULL;

    xSemaphoreTake(file->card->lock, portMAX_DELAY);
    file->closed = true;
    sd_detach_file_node(file->card, file);
    xSemaphoreGive(file->card->lock);

    xSemaphoreGive(file->lock);
    vSemaphoreDelete(file->lock);

    int32_t error = close_result == 0 ? ESPIO_SD_OK : sd_map_errno(close_errno);
    free(file);
    
    if (error != ESPIO_SD_OK) {
        ESPIO_SD_RETURN(error, close_errno, "failed to close file");
    }
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Read bytes from an open SD-backed file.
 */
int32_t espio_sd_file_read(espio_sd_file_t* file, void* buffer, size_t size, size_t* out_bytes_read) {
    if (!file || !buffer) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file or buffer");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file not open");
    }

    errno = 0;
    size_t bytes_read = fread(buffer, 1U, size, file->stream);
    if (out_bytes_read) {
        *out_bytes_read = bytes_read;
    }

    if (bytes_read < size && ferror(file->stream)) {
        int read_errno = errno;
        clearerr(file->stream);
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(sd_map_errno(read_errno), read_errno, "read error");
    }

    xSemaphoreGive(file->lock);
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Write bytes to an open SD-backed file.
 */
int32_t espio_sd_file_write(espio_sd_file_t* file, const void* buffer, size_t size, size_t* out_bytes_written) {
    if (!file || !buffer) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file or buffer");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file not open");
    }

    errno = 0;
    size_t bytes_written = fwrite(buffer, 1U, size, file->stream);
    if (out_bytes_written) {
        *out_bytes_written = bytes_written;
    }

    if (bytes_written != size) {
        int write_errno = errno;
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(sd_map_errno(write_errno), write_errno, "write error");
    }

    xSemaphoreGive(file->lock);
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Flush pending buffered data to the filesystem.
 */
int32_t espio_sd_file_sync(espio_sd_file_t* file) {
    if (!file) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file not open");
    }

    errno = 0;
    int flush_result = fflush(file->stream);
    int flush_errno = errno;
    xSemaphoreGive(file->lock);

    if (flush_result != 0) {
        ESPIO_SD_RETURN(sd_map_errno(flush_errno), flush_errno, "flush failed");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Reposition the file cursor.
 */
int32_t espio_sd_file_seek(espio_sd_file_t* file, int64_t offset, espio_sd_seek_origin_t origin) {
    if (!file) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file");
    }

    int libc_origin = sd_seek_origin_to_libc(origin);
    if (libc_origin < 0) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "invalid seek origin");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file not open");
    }

    errno = 0;
    int seek_result = fseek(file->stream, (long)offset, libc_origin);
    int seek_errno = errno;
    xSemaphoreGive(file->lock);

    if (seek_result != 0) {
        ESPIO_SD_RETURN(sd_map_errno(seek_errno), seek_errno, "seek failed");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Query the current file cursor position.
 */
int32_t espio_sd_file_tell(espio_sd_file_t* file, int64_t* out_offset) {
    if (!file || !out_offset) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file or out_offset");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file not open");
    }

    errno = 0;
    long position = ftell(file->stream);
    int tell_errno = errno;
    xSemaphoreGive(file->lock);

    if (position < 0L) {
        ESPIO_SD_RETURN(sd_map_errno(tell_errno), tell_errno, "ftell failed");
    }

    *out_offset = (int64_t)position;
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Query the total file size without changing the final cursor position.
 */
int32_t espio_sd_file_size(espio_sd_file_t* file, int64_t* out_size) {
    if (!file || !out_size) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null file or out_size");
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    if (file->closed || !file->stream) {
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "file not open");
    }

    errno = 0;
    long current = ftell(file->stream);
    if (current < 0L) {
        int tell_errno = errno;
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(sd_map_errno(tell_errno), tell_errno, "ftell failed");
    }

    if (fseek(file->stream, 0L, SEEK_END) != 0) {
        int seek_errno = errno;
        (void)fseek(file->stream, current, SEEK_SET);
        xSemaphoreGive(file->lock);
        ESPIO_SD_RETURN(sd_map_errno(seek_errno), seek_errno, "seek failed");
    }

    long end = ftell(file->stream);
    int end_errno = errno;
    (void)fseek(file->stream, current, SEEK_SET);
    xSemaphoreGive(file->lock);

    if (end < 0L) {
        ESPIO_SD_RETURN(sd_map_errno(end_errno), end_errno, "ftell failed");
    }

    *out_size = (int64_t)end;
    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Check whether the file cursor reached end-of-file.
 */
bool espio_sd_file_eof(espio_sd_file_t* file) {
    if (!file) {
        return false;
    }

    xSemaphoreTake(file->lock, portMAX_DELAY);
    bool is_eof = !file->closed && file->stream && feof(file->stream);
    xSemaphoreGive(file->lock);
    return is_eof;
}

/**
 * @brief Check whether a relative path exists inside the mounted filesystem.
 */
int32_t espio_sd_path_exists(espio_sd_t* card, const char* logical_path, bool* out_exists) {
    if (!card || !out_exists) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card or out_exists");
    }

    char* full_path = sd_resolve_path(card, logical_path);
    if (!full_path) {
        return espio_err_get_last()->code;
    }

    xSemaphoreTake(card->lock, portMAX_DELAY);
    if (!card->mounted || card->unmount_requested) {
        xSemaphoreGive(card->lock);
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_SD_ERR_NOT_MOUNTED, 0, "card not mounted");
    }

    struct stat path_stat = {0};
    errno = 0;
    int stat_result = stat(full_path, &path_stat);
    int stat_errno = errno;
    xSemaphoreGive(card->lock);
    free(full_path);

    if (stat_result == 0) {
        *out_exists = true;
        ESPIO_SD_RETURN_OK();
    }

    if (stat_errno == ENOENT) {
        *out_exists = false;
        ESPIO_SD_RETURN_OK();
    }

    ESPIO_SD_RETURN(sd_map_errno(stat_errno), stat_errno, "stat failed");
}

/**
 * @brief Create a directory relative to the configured mount point.
 */
int32_t espio_sd_mkdir(espio_sd_t* card, const char* logical_path) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    char* full_path = sd_resolve_path(card, logical_path);
    if (!full_path) {
        return espio_err_get_last()->code;
    }

    xSemaphoreTake(card->lock, portMAX_DELAY);
    if (!card->mounted || card->unmount_requested) {
        xSemaphoreGive(card->lock);
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_SD_ERR_NOT_MOUNTED, 0, "card not mounted");
    }

    errno = 0;
    int mkdir_result = mkdir(full_path, 0775);
    int mkdir_errno = errno;
    xSemaphoreGive(card->lock);
    free(full_path);

    if (mkdir_result != 0) {
        ESPIO_SD_RETURN(sd_map_errno(mkdir_errno), mkdir_errno, "mkdir failed");
    }

    ESPIO_SD_RETURN_OK();
}

/**
 * @brief Remove a file or empty directory relative to the mount point.
 */
int32_t espio_sd_remove(espio_sd_t* card, const char* logical_path) {
    if (!card) {
        ESPIO_SD_RETURN(ESPIO_INVALID_ARG, 0, "null card");
    }

    char* full_path = sd_resolve_path(card, logical_path);
    if (!full_path) {
        return espio_err_get_last()->code;
    }

    xSemaphoreTake(card->lock, portMAX_DELAY);
    if (!card->mounted || card->unmount_requested) {
        xSemaphoreGive(card->lock);
        free(full_path);
        ESPIO_SD_RETURN(ESPIO_SD_ERR_NOT_MOUNTED, 0, "card not mounted");
    }

    errno = 0;
    int remove_result = remove(full_path);
    int remove_errno = errno;
    xSemaphoreGive(card->lock);
    free(full_path);

    if (remove_result != 0) {
        ESPIO_SD_RETURN(sd_map_errno(remove_errno), remove_errno, "remove failed");
    }

    ESPIO_SD_RETURN_OK();
}
