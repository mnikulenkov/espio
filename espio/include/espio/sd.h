/**
 * @file sd.h
 * @brief SD card abstraction API built on top of ESP-IDF v5 storage drivers.
 *
 * This header exposes an application-facing facade for mounting an SD card,
 * opening files relative to the configured mount point, and coordinating safe
 * unmount behavior across multiple tasks.
 */

#ifndef ESPIO_SD_H
#define ESPIO_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "espio/sd_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque SD card handle.
 */
typedef struct espio_sd espio_sd_t;

/**
 * @brief Opaque SD-backed file handle.
 */
typedef struct espio_sd_file espio_sd_file_t;

/**
 * @brief Public sentinel for an unused GPIO pin.
 */
#define ESPIO_SD_PIN_UNUSED (-1)

/**
 * @brief Default mount point used when configuration leaves it empty.
 */
#define ESPIO_SD_MOUNT_POINT_DEFAULT "/sdcard"

/**
 * @brief Default maximum number of simultaneously open files.
 */
#define ESPIO_SD_MAX_OPEN_FILES_DEFAULT 4U

/**
 * @brief Transport mode used to reach the card.
 */
typedef enum {
    ESPIO_SD_BUS_MODE_SDMMC = 0, ///< Native SDMMC host mode.
    ESPIO_SD_BUS_MODE_SPI = 1    ///< SPI-based SD card mode.
} espio_sd_bus_mode_t;

/**
 * @brief File-open policy exposed by the SD API.
 */
typedef enum {
    ESPIO_SD_OPEN_MODE_READ = 0,              ///< Open an existing file for reading.
    ESPIO_SD_OPEN_MODE_WRITE_TRUNCATE = 1,    ///< Create or truncate a file for writing.
    ESPIO_SD_OPEN_MODE_WRITE_APPEND = 2,      ///< Create or append to a file.
    ESPIO_SD_OPEN_MODE_READWRITE = 3,         ///< Open an existing file for reading and writing.
    ESPIO_SD_OPEN_MODE_READWRITE_CREATE = 4   ///< Create if needed, then allow read and write.
} espio_sd_open_mode_t;

/**
 * @brief File-seek origin used by sd_file_seek().
 */
typedef enum {
    ESPIO_SD_SEEK_SET = 0, ///< Seek relative to the start of the file.
    ESPIO_SD_SEEK_CUR = 1, ///< Seek relative to the current file position.
    ESPIO_SD_SEEK_END = 2  ///< Seek relative to the end of the file.
} espio_sd_seek_origin_t;

/**
 * @brief Filesystem mount options.
 */
typedef struct {
    const char* mount_point;       ///< Requested mount point, defaults to `ESPIO_SD_MOUNT_POINT_DEFAULT`.
    size_t max_open_files;         ///< Open-file limit for both VFS and the facade.
    bool format_if_mount_failed;   ///< Format the card if the FAT mount fails.
    size_t allocation_unit_size;   ///< FAT allocation unit used during format, or 0 for default.
    bool disk_status_check;        ///< Enable ff_disk_status checks for hot-removal diagnostics.
} espio_sd_mount_config_t;

/**
 * @brief Optional SD power-switch configuration.
 */
typedef struct {
    int power_gpio;                ///< GPIO controlling card power, or `ESPIO_SD_PIN_UNUSED`.
    bool power_active_high;        ///< True when the GPIO level high enables power.
    uint32_t power_on_delay_ms;    ///< Delay after enabling power before probing the card.
    uint32_t power_off_delay_ms;   ///< Delay after disabling power during shutdown.
} espio_sd_power_config_t;

/**
 * @brief Native SDMMC bus configuration.
 */
typedef struct {
    int slot;                      ///< SDMMC host slot index passed to ESP-IDF.
    uint8_t width;                 ///< Requested bus width, usually 1 or 4.
    int cd_pin;                    ///< Optional card-detect GPIO, or `ESPIO_SD_PIN_UNUSED`.
    int wp_pin;                    ///< Optional write-protect GPIO, or `ESPIO_SD_PIN_UNUSED`.
    bool enable_internal_pullups;  ///< Enable debug-only internal pull-ups on active SDMMC pins.
} espio_sd_sdmmc_config_t;

/**
 * @brief SPI-based SD bus configuration.
 */
typedef struct {
    int host_id;                   ///< SPI host controller identifier.
    int sck_pin;                   ///< SPI clock GPIO.
    int mosi_pin;                  ///< SPI MOSI / CMD GPIO.
    int miso_pin;                  ///< SPI MISO / DAT0 GPIO.
    int cs_pin;                    ///< SPI CS / DAT3 GPIO.
    int cd_pin;                    ///< Optional card-detect GPIO, or `ESPIO_SD_PIN_UNUSED`.
    int wp_pin;                    ///< Optional write-protect GPIO, or `ESPIO_SD_PIN_UNUSED`.
    uint32_t max_freq_khz;         ///< Requested upper-bound SPI clock frequency.
    bool bus_managed_by_sd;        ///< True when the facade should initialize and free the SPI bus.
    bool enable_internal_pullups;  ///< Enable host-side pull-ups on active SPI pins.
} espio_sd_spi_config_t;

/**
 * @brief Top-level SD card configuration.
 */
typedef struct {
    espio_sd_bus_mode_t mode;      ///< Selected host transport.
    espio_sd_mount_config_t mount; ///< Filesystem and VFS policy.
    espio_sd_power_config_t power; ///< Optional board-level power switching.
    union {
        espio_sd_sdmmc_config_t sdmmc; ///< Native SDMMC settings.
        espio_sd_spi_config_t spi;     ///< SPI transport settings.
    } bus;
} espio_sd_config_t;

/**
 * @brief Convenience defaults for mount behavior.
 */
#define ESPIO_SD_MOUNT_CONFIG_DEFAULT() \
    { \
        .mount_point = ESPIO_SD_MOUNT_POINT_DEFAULT, \
        .max_open_files = ESPIO_SD_MAX_OPEN_FILES_DEFAULT, \
        .format_if_mount_failed = false, \
        .allocation_unit_size = 0U, \
        .disk_status_check = false, \
    }

/**
 * @brief Convenience defaults for power switching.
 */
#define ESPIO_SD_POWER_CONFIG_DEFAULT() \
    { \
        .power_gpio = ESPIO_SD_PIN_UNUSED, \
        .power_active_high = true, \
        .power_on_delay_ms = 0U, \
        .power_off_delay_ms = 0U, \
    }

/**
 * @brief Convenience defaults for native SDMMC operation.
 */
#define ESPIO_SD_SDMMC_CONFIG_DEFAULT() \
    { \
        .slot = 1, \
        .width = 4U, \
        .cd_pin = ESPIO_SD_PIN_UNUSED, \
        .wp_pin = ESPIO_SD_PIN_UNUSED, \
        .enable_internal_pullups = false, \
    }

/**
 * @brief Convenience defaults for SPI-based SD operation.
 */
#define ESPIO_SD_SPI_CONFIG_DEFAULT() \
    { \
        .host_id = 2, \
        .sck_pin = ESPIO_SD_PIN_UNUSED, \
        .mosi_pin = ESPIO_SD_PIN_UNUSED, \
        .miso_pin = ESPIO_SD_PIN_UNUSED, \
        .cs_pin = ESPIO_SD_PIN_UNUSED, \
        .cd_pin = ESPIO_SD_PIN_UNUSED, \
        .wp_pin = ESPIO_SD_PIN_UNUSED, \
        .max_freq_khz = 20000U, \
        .bus_managed_by_sd = false, \
        .enable_internal_pullups = false, \
    }

/**
 * @brief Convenience defaults for the complete SDMMC-based facade.
 */
#define ESPIO_SD_CONFIG_DEFAULT_SDMMC() \
    { \
        .mode = ESPIO_SD_BUS_MODE_SDMMC, \
        .mount = ESPIO_SD_MOUNT_CONFIG_DEFAULT(), \
        .power = ESPIO_SD_POWER_CONFIG_DEFAULT(), \
        .bus.sdmmc = ESPIO_SD_SDMMC_CONFIG_DEFAULT(), \
    }

/**
 * @brief Convenience defaults for the complete SPI-based facade.
 */
#define ESPIO_SD_CONFIG_DEFAULT_SPI() \
    { \
        .mode = ESPIO_SD_BUS_MODE_SPI, \
        .mount = ESPIO_SD_MOUNT_CONFIG_DEFAULT(), \
        .power = ESPIO_SD_POWER_CONFIG_DEFAULT(), \
        .bus.spi = ESPIO_SD_SPI_CONFIG_DEFAULT(), \
    }

/**
 * @brief Create, configure, and mount an SD card facade.
 *
 * @param out_card Output location for the created handle.
 * @param config SD card configuration.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_create(espio_sd_t** out_card, const espio_sd_config_t* config);

/**
 * @brief Mount an existing SD card facade after sd_unmount().
 *
 * @param card SD card handle.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_mount(espio_sd_t* card);

/**
 * @brief Unmount an SD card and release the active filesystem registration.
 *
 * @param card SD card handle.
 * @param timeout_ms Maximum time to wait for open files to close. Zero means do not wait.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_unmount(espio_sd_t* card, uint32_t timeout_ms);

/**
 * @brief Destroy an SD card facade after ensuring it is unmounted.
 *
 * @param card SD card handle.
 * @return SD_OK on success, or an error code when cleanup could not finish.
 *         Use err_get_last() for context.
 */
int32_t espio_sd_destroy(espio_sd_t* card);

/**
 * @brief Check whether the card is currently mounted.
 *
 * @param card SD card handle.
 * @return True when mounted, false otherwise.
 */
bool espio_sd_is_mounted(const espio_sd_t* card);

/**
 * @brief Get the normalized mount point used by the facade.
 *
 * @param card SD card handle.
 * @return Immutable mount-point string, or NULL on invalid input.
 */
const char* espio_sd_get_mount_point(const espio_sd_t* card);

/**
 * @brief Open a file relative to the configured mount point.
 *
 * @param card SD card handle.
 * @param logical_path Relative path inside the mounted filesystem.
 * @param mode Open mode.
 * @param out_file Output location for the created file handle.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_open(espio_sd_t* card, const char* logical_path, espio_sd_open_mode_t mode, espio_sd_file_t** out_file);

/**
 * @brief Close a file handle and flush any pending buffered writes.
 *
 * @param file File handle.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_close(espio_sd_file_t* file);

/**
 * @brief Read bytes from an open SD-backed file.
 *
 * @param file File handle.
 * @param buffer Output buffer.
 * @param size Requested number of bytes.
 * @param out_bytes_read Output number of bytes read, when non-NULL.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_read(espio_sd_file_t* file, void* buffer, size_t size, size_t* out_bytes_read);

/**
 * @brief Write bytes to an open SD-backed file.
 *
 * @param file File handle.
 * @param buffer Input buffer.
 * @param size Requested number of bytes.
 * @param out_bytes_written Output number of bytes written, when non-NULL.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_write(espio_sd_file_t* file, const void* buffer, size_t size, size_t* out_bytes_written);

/**
 * @brief Flush pending buffered data to the filesystem.
 *
 * @param file File handle.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_sync(espio_sd_file_t* file);

/**
 * @brief Reposition the file cursor.
 *
 * @param file File handle.
 * @param offset Signed byte offset.
 * @param origin Seek origin.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_seek(espio_sd_file_t* file, int64_t offset, espio_sd_seek_origin_t origin);

/**
 * @brief Query the current file cursor position.
 *
 * @param file File handle.
 * @param out_offset Output current file offset.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_tell(espio_sd_file_t* file, int64_t* out_offset);

/**
 * @brief Query the total file size without changing the final cursor position.
 *
 * @param file File handle.
 * @param out_size Output file size in bytes.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_file_size(espio_sd_file_t* file, int64_t* out_size);

/**
 * @brief Check whether the file cursor reached end-of-file.
 *
 * @param file File handle.
 * @return True when EOF is set, false otherwise.
 */
bool espio_sd_file_eof(espio_sd_file_t* file);

/**
 * @brief Check whether a relative path exists inside the mounted filesystem.
 *
 * @param card SD card handle.
 * @param logical_path Relative path inside the mounted filesystem.
 * @param out_exists Output existence flag.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_path_exists(espio_sd_t* card, const char* logical_path, bool* out_exists);

/**
 * @brief Create a directory relative to the configured mount point.
 *
 * @param card SD card handle.
 * @param logical_path Relative directory path.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_mkdir(espio_sd_t* card, const char* logical_path);

/**
 * @brief Remove a file or empty directory relative to the mount point.
 *
 * @param card SD card handle.
 * @param logical_path Relative path to remove.
 * @return SD_OK on success, or an error code on failure.Use err_get_last() for context.
 */
int32_t espio_sd_remove(espio_sd_t* card, const char* logical_path);

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_SD_H */
