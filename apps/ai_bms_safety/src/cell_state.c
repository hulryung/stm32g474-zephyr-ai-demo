#include "cell_state.h"

#include <zephyr/kernel.h>
#include <string.h>

static struct cell_snapshot g_snap;
static struct status_flags  g_flags;
static struct k_spinlock    g_lock;

void cell_state_set(const struct cell_snapshot *s) {
    k_spinlock_key_t k = k_spin_lock(&g_lock);
    g_snap = *s;
    k_spin_unlock(&g_lock, k);
}

void cell_state_get(struct cell_snapshot *out) {
    k_spinlock_key_t k = k_spin_lock(&g_lock);
    *out = g_snap;
    k_spin_unlock(&g_lock, k);
}

void status_flags_get(struct status_flags *out) {
    k_spinlock_key_t k = k_spin_lock(&g_lock);
    *out = g_flags;
    k_spin_unlock(&g_lock, k);
}

/* internal — used by safety/ml threads to publish */
void status_flags_set(const struct status_flags *new_flags) {
    k_spinlock_key_t k = k_spin_lock(&g_lock);
    g_flags = *new_flags;
    k_spin_unlock(&g_lock, k);
}
