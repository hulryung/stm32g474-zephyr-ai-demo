/* Generated — DO NOT EDIT */
#ifndef SOC_CALIB_H
#define SOC_CALIB_H

#define SOC_WINDOW 32

#ifdef __cplusplus
extern "C" {
#endif

/* SOC-OCV lookup table (sorted by SOC in 0..100 %). */
extern const unsigned int g_sococv_n;
extern const float        g_sococv_soc[];
extern const float        g_sococv_ocv[];

/* Single-RC ECM parameters fit from training cells. */
extern const float g_ecm_R0;
extern const float g_ecm_R1;
extern const float g_ecm_C1;
extern const float g_ecm_Qnom;

/* Embedded test sample windows (from B0018 holdout cell). */
struct soc_sample {
    const char  *name;
    const char  *cell;
    float        capacity_ah;
    float        v[SOC_WINDOW];
    float        i[SOC_WINDOW];
    float        T[SOC_WINDOW];
    float        soc_true[SOC_WINDOW];
};

extern const unsigned int g_sample_count;
extern const struct soc_sample g_samples[];

/* Normalization constants used by the int8 ML models (must match train.py). */
#define SOC_V_MIN  2.4f
#define SOC_V_MAX  4.3f
#define SOC_I_MIN  -3.0f
#define SOC_I_MAX  3.0f
#define SOC_T_MIN  20.0f
#define SOC_T_MAX  45.0f
#define SOC_NORM   100.0f

#ifdef __cplusplus
}
#endif
#endif
