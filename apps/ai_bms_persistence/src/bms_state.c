#include "bms_state.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(bms_state, LOG_LEVEL_INF);

#define KEY_PREFIX "bms"
#define KEY_FULL   KEY_PREFIX "/state"

static struct bms_persisted_state g_state;
static bool g_loaded = false;

/* settings handler — called by settings_load() for each matching key */
static int handle_set(const char *name, size_t len,
                      settings_read_cb read_cb, void *cb_arg)
{
    /* Only one key in this subtree, so name should be empty here. */
    if (len != sizeof(g_state)) {
        LOG_WRN("saved bms state size %u != current schema %u",
                (unsigned)len, (unsigned)sizeof(g_state));
        return -EINVAL;
    }
    int rc = read_cb(cb_arg, &g_state, sizeof(g_state));
    if (rc < 0) {
        LOG_ERR("settings read failed: %d", rc);
        return rc;
    }
    if (g_state.version != BMS_STATE_VERSION) {
        LOG_WRN("schema version mismatch: stored=%u current=%u",
                g_state.version, BMS_STATE_VERSION);
        memset(&g_state, 0, sizeof(g_state));
        return -EINVAL;
    }
    g_loaded = true;
    LOG_INF("restored from NVS: SOC=%.2f%% Q=%.4fAh boot#%u",
            (double)g_state.soc_pct,
            (double)g_state.Q_estimate_ah,
            g_state.boot_count);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(bms_state, KEY_PREFIX, NULL,
                               handle_set, NULL, NULL);

int bms_state_init(void)
{
    int rc = settings_subsys_init();
    if (rc) { LOG_ERR("settings_subsys_init: %d", rc); return rc; }
    rc = settings_load_subtree(KEY_PREFIX);
    if (rc) { LOG_WRN("settings_load: %d", rc); }
    /* Increment boot counter even if there was nothing to load */
    g_state.boot_count++;
    return 0;
}

const struct bms_persisted_state *bms_state_get(void)
{
    return g_loaded ? &g_state : NULL;
}

int bms_state_set(const struct bms_persisted_state *s)
{
    g_state = *s;
    g_state.version = BMS_STATE_VERSION;
    g_loaded = true;
    int rc = settings_save_one(KEY_FULL, &g_state, sizeof(g_state));
    if (rc) { LOG_ERR("settings_save_one: %d", rc); return rc; }
    return 0;
}

int bms_state_clear(void)
{
    int rc = settings_delete(KEY_FULL);
    memset(&g_state, 0, sizeof(g_state));
    g_loaded = false;
    return rc;
}
