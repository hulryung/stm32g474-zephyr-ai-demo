/*
 * BMS state envelope — what survives a power cycle.
 *
 * In a real product this lives in NVS (flash) on every BMS shutdown and
 * is restored on every boot. We use Zephyr's settings_subsys (FCB or NVS
 * backend) to handle the wear-leveling.
 */

#ifndef BMS_STATE_H
#define BMS_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BMS_STATE_VERSION 1u

struct bms_persisted_state {
    uint32_t version;          /* schema version, lets us evolve later */
    uint32_t boot_count;       /* incremented every boot */
    uint64_t shutdown_uptime_ms; /* uptime at last save */
    uint64_t shutdown_wallclock_s; /* host-supplied wall clock at save (0 if unknown) */

    float    soc_pct;          /* last EKF SOC */
    float    Q_estimate_ah;    /* last capacity estimate */
    float    v_rc;             /* last EKF RC voltage */
    float    P_soc;            /* covariances (so EKF doesn't think it's certain after restart) */
    float    P_Q;
    uint32_t cycles_completed;
};

/* Initialize storage backend; reads any existing state into the global. */
int bms_state_init(void);

/* Read the currently held state (returns NULL if no saved state yet). */
const struct bms_persisted_state *bms_state_get(void);

/* Update fields and persist. */
int bms_state_set(const struct bms_persisted_state *new_state);

/* Erase the saved state (factory reset / cell swap). */
int bms_state_clear(void);

#ifdef __cplusplus
}
#endif
#endif
