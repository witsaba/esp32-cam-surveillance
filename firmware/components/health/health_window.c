/* health_window.c — FW-16.1 pure sliding-window implementation.
 *
 * See health_window.h for the state model (design AD3). This TU
 * deliberately includes NOTHING from IDF: the host Unity suite
 * compiles it with plain gcc, and the device build links it into
 * the health component unchanged.
 */
#include "health_window.h"

/* Drop entries strictly older than now_us − window_us from the
 * head. Timestamps are monotonic, so pruning stops at the first
 * in-window entry. An entry EXACTLY at now_us − window_us is
 * kept (boundary pinned by test_soft_recovery_window.c S5). */
static void prune_stale(health_window_t *w, int64_t now_us,
                        int64_t window_us)
{
    const int64_t cutoff = now_us - window_us;
    while (w->count > 0 && w->ts_us[w->head] < cutoff) {
        w->head = (w->head + 1u) % HEALTH_WINDOW_CAP;
        w->count--;
    }
}

void health_window_reset(health_window_t *w)
{
    if (!w) return;
    w->head         = 0;
    w->count        = 0;
    w->episode_open = false;
}

void health_window_mark_reconnected(health_window_t *w)
{
    if (!w) return;
    w->episode_open = false;
}

size_t health_window_record(health_window_t *w, int64_t now_us,
                            int64_t window_us)
{
    if (!w || window_us <= 0) {
        return w ? w->count : 0;
    }

    /* Episode latch (AD2): retries inside an open outage episode
     * are swallowed BEFORE any pruning/append work — one physical
     * fault advances the counter by exactly one. The initial
     * latch is closed (reset), so a drop observed before any
     * GOT_IP still counts. */
    if (w->episode_open) {
        return w->count;
    }

    /* Lazy prune BEFORE every ingest/evaluation (AD3). */
    prune_stale(w, now_us, window_us);

    if (w->count < HEALTH_WINDOW_CAP) {
        w->ts_us[(w->head + w->count) % HEALTH_WINDOW_CAP] = now_us;
        w->count++;
    }
    /* else: ring full — the trigger has long fired at any legal
     * threshold (cap == Kconfig range ceiling); wrap is a no-op. */

    w->episode_open = true; /* the outage episode opens now */
    return w->count;
}

bool health_window_should_recover(const health_window_t *w,
                                  uint32_t threshold)
{
    if (!w) return false;
    return (uint32_t)w->count >= threshold;
}

size_t health_window_count(const health_window_t *w)
{
    return w ? w->count : 0;
}
