/* health_window.h — FW-16.1 pure sliding-window threshold core
 * (R-FW16-1.1).
 *
 * Zero-IDF-deps module: no FreeRTOS, no esp_timer, no NVS. Every
 * function takes or returns plain values so host Unity tests drive
 * explicit microsecond timestamps directly (design AD3). The
 * production clock enters only in components/health/health.c,
 * which calls esp_timer_get_time() and feeds this API.
 *
 * State model:
 *   - a fixed-capacity ring buffer of per-failure µs timestamps;
 *     timestamps are monotonic (esp_timer_get_time), so the ring
 *     stays time-ordered by construction;
 *   - LAZY PRUNE before EVERY evaluation: entries strictly older
 *     than now_us − window_us are dropped on ingest (equality at
 *     the boundary is kept);
 *   - trigger rule: fire iff in-window count >= threshold.
 *
 * Capacity: HEALTH_WINDOW_CAP == the Kconfig FAILS range ceiling
 * (main/Kconfig.projbuild:53 range 5..200). The trigger fires at
 * count >= threshold <= 200, so the ring can never overflow on a
 * live threshold; a full ring simply wraps (oldest entry reused).
 */
#ifndef HEALTH_WINDOW_H
#define HEALTH_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ring capacity == Kconfig CONFIG_FIRMWARE_SOFT_RECOVERY_FAILS
 * range ceiling (200). See file comment. */
#define HEALTH_WINDOW_CAP 200u

/* Window state. Callers embed this by value (host tests stack-
 * allocate it; health.c owns one file-static instance). */
typedef struct {
    int64_t ts_us[HEALTH_WINDOW_CAP]; /* failure timestamps (µs)  */
    size_t  head;                     /* oldest-entry index       */
    size_t  count;                    /* retained entries         */
    bool    episode_open;             /* outage-episode latch
                                       * (AD2): true between the
                                       * first DISCONNECTED of an
                                       * outage and the next GOT_IP */
} health_window_t;

/* Drop every recorded failure AND close any open episode. */
void health_window_reset(health_window_t *w);

/* Episode-latched ingestion (AD2): while an outage episode is
 * open, paired DISCONNECTED retries are swallowed — no append,
 * no increment. The FIRST ingest with the latch closed appends
 * ONE timestamp and opens the episode. Returns the in-window
 * count after lazy-prune (+ possible append). This is the single
 * ingestion entry point — every evaluation happens through it. */
size_t health_window_record(health_window_t *w,
                            int64_t now_us,
                            int64_t window_us);

/* GOT_IP episode-boundary marker: closes the open episode so the
 * next drop counts as a NEW failure. Safe when already closed. */
void health_window_mark_reconnected(health_window_t *w);

/* Trigger decision over the CURRENT window content: true iff
 * in-window count >= threshold. Pure read — no pruning here. */
bool health_window_should_recover(const health_window_t *w,
                                  uint32_t threshold);

/* Number of retained (in-window) failures. */
size_t health_window_count(const health_window_t *w);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_WINDOW_H */
