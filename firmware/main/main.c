/* main.c — ESP-IDF firmware entry point.
 *
 * FW-02 commit 1 (bootstrap) lands the minimal app_main that
 * initialises NVS and logs the result. Commit 2 extends it to call
 * `config_load()` once the `config` component is in place. Commit 5
 * further extends it with an `nvs_get_stats()` smoke that proves
 * the partition sizing acceptance gate from
 * `docs/firmware-milestones.md` FW-02.4.
 */
#include <stdbool.h>

#include "config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "fw";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();

    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);
    ESP_LOGI(TAG,
             "nvs_flash_init ret=%d config_load status=%d dirty=%d ssid='%s'",
             ret, (int)st, dirty ? 1 : 0, cfg.wifi.ssid);
}