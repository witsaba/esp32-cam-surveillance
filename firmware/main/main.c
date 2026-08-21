/* main.c — ESP-IDF firmware entry point
 *
 * FW-02 commit 1 (bootstrap): initialise NVS and log the result.
 *
 * Commit 2 will uncomment the `config_load()` call once the `config`
 * component is in place (see `firmware/components/config/config.h`).
 *
 * Commit 5 will extend this entry point with an `nvs_get_stats()`
 * smoke that proves the partition sizing acceptance gate from
 * `docs/firmware-milestones.md` FW-02.4.
 */
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "fw";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    /* FW-02 commit 2 will add:
     *   #include "config.h"
     *   config_t cfg = {0};
     *   bool dirty = false;
     *   config_status_t st = config_load(&cfg, &dirty);
     *   ESP_LOGI(TAG, "config_load status=%d dirty=%d", st, dirty);
     */

    ESP_LOGI(TAG, "nvs_flash_init ret=%d", ret);
}