#include "sd_card.h"

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "sd_card";

/* Dedicated SPI bus for the TF slot - see sd_card.h. */
#define SD_SPI_HOST     SPI2_HOST
#define SD_MOSI_GPIO    11
#define SD_MISO_GPIO    13
#define SD_SCLK_GPIO    12
/* SD_CS is CH422G_EXIO_SD_CS (EXIO4 on the expander), not a GPIO number -
 * see ch422g.h and the CS-handling comment in sd_card.h. */

/* Conservative default; raise if you profile large sequential ROM reads
 * and want fewer, bigger DMA transfers - just keep it under the SPI
 * driver's DMA transfer-size ceiling. */
#define SD_MAX_TRANSFER_SZ  4000

static sdmmc_card_t *s_card;
static ch422g_handle_t s_expander;
static bool s_spi_bus_initialized;

esp_err_t sd_card_init(ch422g_handle_t expander)
{
    if (expander == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_card != NULL) {
        ESP_LOGW(TAG, "already mounted");
        return ESP_OK;
    }
    s_expander = expander;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SD_MAX_TRANSFER_SZ,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spi_bus_initialized = true;

    /* Select the card for the whole session - see sd_card.h. Active low
     * per the CH422G_EXIO_SD_CS comment in ch422g.h. */
    err = ch422g_set_level(s_expander, CH422G_EXIO_SD_CS, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to assert SD_CS via CH422G: %s", esp_err_to_name(err));
        goto fail_free_bus;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SD_SPI_HOST;
    slot_cfg.gpio_cs = GPIO_NUM_NC; /* CS handled externally via the CH422G, above */

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false, /* deliberately off - see header:
                                             flipping this to true on a
                                             card with an unreadable
                                             filesystem will silently wipe
                                             it. Format the card on a PC
                                             first if mounting fails. */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot_cfg,
                                   &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "failed to mount FAT filesystem on the TF card - "
                          "reformat it as FAT32 on a PC, or set "
                          "format_if_mount_failed=true above if you're okay "
                          "with the card being erased automatically");
        } else {
            ESP_LOGE(TAG, "card init failed: %s - check the card is seated "
                          "and that GPIO11/12/13 + CH422G EXIO4 match your "
                          "board revision", esp_err_to_name(err));
        }
        ch422g_set_level(s_expander, CH422G_EXIO_SD_CS, true);
        goto fail_free_bus;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "TF card mounted at %s", SD_CARD_MOUNT_POINT);
    return ESP_OK;

fail_free_bus:
    spi_bus_free(SD_SPI_HOST);
    s_spi_bus_initialized = false;
    return err;
}

void sd_card_deinit(void)
{
    if (s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    if (s_expander != NULL) {
        ch422g_set_level(s_expander, CH422G_EXIO_SD_CS, true);
    }
    if (s_spi_bus_initialized) {
        spi_bus_free(SD_SPI_HOST);
        s_spi_bus_initialized = false;
    }
}
