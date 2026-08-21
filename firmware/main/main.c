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

    /* FW-02.4 device-side partition sizing smoke. After NVS init
     * (and after writing every field of config_t in the FW-02.1
     * round-trip flow), this prints the live entry counts. The
     * companion static-math check is done in code review against
     * `firmware/partitions.csv` (24 KB NVS holds config_t ~512 B
     * + camera_cfg ~64 B at < 2 % usage). */
    nvs_stats_t nvs_stats = {0};
    esp_err_t stats_ret = nvs_get_stats(NULL, &nvs_stats);
    ESP_LOGI(TAG,
             "nvs_flash_init ret=%d stats_ret=%d used_entries=%lu "
             "free_entries=%lu total_entries=%lu namespace_count=%lu",
             ret, stats_ret,
             (unsigned long)nvs_stats.used_entries,
             (unsigned long)nvs_stats.free_entries,
             (unsigned long)nvs_stats.total_entries,
             (unsigned long)nvs_stats.namespace_count);

    config_t cfg;
    bool dirty = false;
    config_status_t st = config_load(&cfg, &dirty);
    ESP_LOGI(TAG,
             "config_load status=%d dirty=%d ssid='%s'",
             (int)st, dirty ? 1 : 0, cfg.wifi.ssid);
}