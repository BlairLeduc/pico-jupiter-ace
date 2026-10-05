/* handoff.c — what the two cores share (design.md §4.3, §4.4). */

#include "handoff.h"

#include "hardware/sync.h"

snappool_t g_pool;
static spin_lock_t *s_lock;

boot_report_t g_boot;
volatile core1_stats_t g_c1 = { .sb_version = -1, .battery = -1, .temp_c = INT32_MIN };
volatile core0_perf_t  g_c0;
ui_t g_ui = { .volume = 8, .fast_tape = true };

void handoff_init(void) {
    snappool_init(&g_pool);
    s_lock = spin_lock_init(spin_lock_claim_unused(true));
}

int pool_claim(void) {
    uint32_t irq = spin_lock_blocking(s_lock);
    int i = snappool_claim(&g_pool);
    spin_unlock(s_lock, irq);
    return i;
}

void pool_publish(int i) {
    uint32_t irq = spin_lock_blocking(s_lock);
    snappool_publish(&g_pool, i);
    spin_unlock(s_lock, irq);
}

int pool_take(void) {
    uint32_t irq = spin_lock_blocking(s_lock);
    int i = snappool_take(&g_pool);
    spin_unlock(s_lock, irq);
    return i;
}

void pool_release(int i) {
    uint32_t irq = spin_lock_blocking(s_lock);
    snappool_release(&g_pool, i);
    spin_unlock(s_lock, irq);
}
