/*
 * ML SOC estimators: MLP, LSTM, hybrid (EKF + MLP residual).
 * Each owns its TFLM interpreter + arena.
 */

extern "C" {
#include "soc_estimators.h"
}
#include "model_mlp.hpp"
#include "model_lstm.hpp"
#include "model_hybrid.hpp"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <math.h>
#include <stdint.h>

/* ---- Common quantize/dequantize helpers ---------------------------------- */

static inline int8_t qf32_to_i8(float x, float scale, int zp)
{
    int32_t q = (int32_t)lroundf(x / scale) + zp;
    if (q < -128) q = -128;
    if (q >  127) q =  127;
    return (int8_t)q;
}

static inline float dq_i8_to_f32(int8_t v, float scale, int zp)
{
    return (float)((int)v - zp) * scale;
}

/* Normalize (V, I, T) to [-1, +1] for ML inputs. */
static inline float norm_v(float v) { return (v - SOC_V_MIN) / (SOC_V_MAX - SOC_V_MIN) * 2.0f - 1.0f; }
static inline float norm_i(float i) { return (i - SOC_I_MIN) / (SOC_I_MAX - SOC_I_MIN) * 2.0f - 1.0f; }
static inline float norm_T(float T) { return (T - SOC_T_MIN) / (SOC_T_MAX - SOC_T_MIN) * 2.0f - 1.0f; }

/* SOC output normalization: model output [-1, +1] → 0..100 % */
static inline float denorm_soc(float y_norm) {
    if (y_norm < -1.0f) y_norm = -1.0f;
    if (y_norm >  1.0f) y_norm =  1.0f;
    return (y_norm + 1.0f) * 0.5f * SOC_NORM;
}

/* =========== 4. MLP ==================================================== */

namespace mlp {
constexpr int kArena = 4 * 1024;
alignas(16) uint8_t arena[kArena];
const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interp = nullptr;
TfLiteTensor *in_t = nullptr;
TfLiteTensor *out_t = nullptr;
bool ready = false;
}

extern "C" int soc_mlp_init(void)
{
    mlp::model = tflite::GetModel(g_model_mlp);
    if (mlp::model->version() != TFLITE_SCHEMA_VERSION) return -1;
    static tflite::MicroMutableOpResolver<3> resolver;
    resolver.AddFullyConnected();
    resolver.AddRelu();
    resolver.AddReshape();
    static tflite::MicroInterpreter interpreter(
        mlp::model, resolver, mlp::arena, mlp::kArena);
    mlp::interp = &interpreter;
    if (mlp::interp->AllocateTensors() != kTfLiteOk) return -2;
    mlp::in_t  = mlp::interp->input(0);
    mlp::out_t = mlp::interp->output(0);
    mlp::ready = true;
    return 0;
}

extern "C" float soc_mlp_estimate(float v, float i, float T)
{
    if (!mlp::ready) return 0.0f;
    float vn = norm_v(v), in_ = norm_i(i), Tn = norm_T(T);
    float s = mlp::in_t->params.scale;
    int   z = mlp::in_t->params.zero_point;
    mlp::in_t->data.int8[0] = qf32_to_i8(vn,  s, z);
    mlp::in_t->data.int8[1] = qf32_to_i8(in_, s, z);
    mlp::in_t->data.int8[2] = qf32_to_i8(Tn,  s, z);
    if (mlp::interp->Invoke() != kTfLiteOk) return 0.0f;
    float y = dq_i8_to_f32(mlp::out_t->data.int8[0],
                            mlp::out_t->params.scale,
                            mlp::out_t->params.zero_point);
    return denorm_soc(y);
}

extern "C" float soc_mlp_window(const struct soc_sample *s)
{
    /* MLP is per-sample. Run on the last sample of the window. */
    return soc_mlp_estimate(s->v[SOC_WINDOW-1],
                             s->i[SOC_WINDOW-1],
                             s->T[SOC_WINDOW-1]);
}

extern "C" uint32_t soc_mlp_arena_size(void) { return mlp::kArena; }

/* =========== 5. LSTM =================================================== */

namespace lstm {
/* Unrolled LSTM at WINDOW=32 with hidden=16 needs space for many
 * intermediate tensors. 16 KB was too small (AllocateTensors failed
 * silently and Invoke returned 0). 64 KB is generous. */
constexpr int kArena = 64 * 1024;
alignas(16) uint8_t arena[kArena];
const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interp = nullptr;
TfLiteTensor *in_t = nullptr;
TfLiteTensor *out_t = nullptr;
bool ready = false;
}

extern "C" int soc_lstm_init(void)
{
    lstm::model = tflite::GetModel(g_model_lstm);
    if (lstm::model->version() != TFLITE_SCHEMA_VERSION) return -1;
    /* Unrolled LSTM at WINDOW=32 produces this op set (verified by
     * inspecting the .tflite): FullyConnected, Mul, Add, Tanh, Logistic,
     * Pack, Unpack, Split, Fill, Shape, StridedSlice, Transpose.
     * Reshape and Relu are kept for the surrounding Dense layers.
     */
    static tflite::MicroMutableOpResolver<14> resolver;
    resolver.AddFullyConnected();
    resolver.AddRelu();
    resolver.AddReshape();
    resolver.AddMul();
    resolver.AddAdd();
    resolver.AddTanh();
    resolver.AddLogistic();
    resolver.AddPack();
    resolver.AddUnpack();
    resolver.AddSplit();
    resolver.AddFill();
    resolver.AddShape();
    resolver.AddStridedSlice();
    resolver.AddTranspose();
    static tflite::MicroInterpreter interpreter(
        lstm::model, resolver, lstm::arena, lstm::kArena);
    lstm::interp = &interpreter;
    if (lstm::interp->AllocateTensors() != kTfLiteOk) {
        MicroPrintf("LSTM AllocateTensors failed");
        return -2;
    }
    lstm::in_t  = lstm::interp->input(0);
    lstm::out_t = lstm::interp->output(0);
    lstm::ready = true;
    return 0;
}

extern "C" float soc_lstm_window(const struct soc_sample *s)
{
    if (!lstm::ready) return 0.0f;
    float scl = lstm::in_t->params.scale;
    int   zp  = lstm::in_t->params.zero_point;
    /* input shape: (1, WINDOW, 3) row-major: [t][feat] */
    int idx = 0;
    for (int t = 0; t < SOC_WINDOW; ++t) {
        lstm::in_t->data.int8[idx++] = qf32_to_i8(norm_v(s->v[t]), scl, zp);
        lstm::in_t->data.int8[idx++] = qf32_to_i8(norm_i(s->i[t]), scl, zp);
        lstm::in_t->data.int8[idx++] = qf32_to_i8(norm_T(s->T[t]), scl, zp);
    }
    if (lstm::interp->Invoke() != kTfLiteOk) return 0.0f;
    float y = dq_i8_to_f32(lstm::out_t->data.int8[0],
                            lstm::out_t->params.scale,
                            lstm::out_t->params.zero_point);
    return denorm_soc(y);
}

extern "C" uint32_t soc_lstm_arena_size(void) { return lstm::kArena; }

/* =========== 6. Hybrid: EKF + MLP residual ============================ */

namespace hyb {
constexpr int kArena = 4 * 1024;
alignas(16) uint8_t arena[kArena];
const tflite::Model *model = nullptr;
tflite::MicroInterpreter *interp = nullptr;
TfLiteTensor *in_t = nullptr;
TfLiteTensor *out_t = nullptr;
bool ready = false;
}

extern "C" int soc_hybrid_init(void)
{
    hyb::model = tflite::GetModel(g_model_hybrid);
    if (hyb::model->version() != TFLITE_SCHEMA_VERSION) return -1;
    static tflite::MicroMutableOpResolver<3> resolver;
    resolver.AddFullyConnected();
    resolver.AddRelu();
    resolver.AddReshape();
    static tflite::MicroInterpreter interpreter(
        hyb::model, resolver, hyb::arena, hyb::kArena);
    hyb::interp = &interpreter;
    if (hyb::interp->AllocateTensors() != kTfLiteOk) return -2;
    hyb::in_t  = hyb::interp->input(0);
    hyb::out_t = hyb::interp->output(0);
    hyb::ready = true;
    return 0;
}

extern "C" float soc_hybrid_window(const struct soc_sample *s, float soc0_pct)
{
    if (!hyb::ready) return 0.0f;
    /* 1) Run EKF to get SOC_ekf */
    float soc_ekf = soc_ekf_window(s, soc0_pct);

    /* 2) Use last-sample inputs + soc_ekf into the residual MLP */
    float v = s->v[SOC_WINDOW-1];
    float i = s->i[SOC_WINDOW-1];
    float T = s->T[SOC_WINDOW-1];
    float vn = norm_v(v), in_ = norm_i(i), Tn = norm_T(T);
    float ekf_n = soc_ekf / SOC_NORM * 2.0f - 1.0f;

    float scl = hyb::in_t->params.scale;
    int   zp  = hyb::in_t->params.zero_point;
    hyb::in_t->data.int8[0] = qf32_to_i8(vn,    scl, zp);
    hyb::in_t->data.int8[1] = qf32_to_i8(in_,   scl, zp);
    hyb::in_t->data.int8[2] = qf32_to_i8(Tn,    scl, zp);
    hyb::in_t->data.int8[3] = qf32_to_i8(ekf_n, scl, zp);
    if (hyb::interp->Invoke() != kTfLiteOk) return soc_ekf;
    float y = dq_i8_to_f32(hyb::out_t->data.int8[0],
                            hyb::out_t->params.scale,
                            hyb::out_t->params.zero_point);
    /* y is a normalized residual: residual_soc = y/2 * SOC_NORM
     * (training normalized as `residual * 2`)
     */
    float residual_pct = y * 0.5f * SOC_NORM;
    float corrected = soc_ekf + residual_pct;
    if (corrected < 0.0f) corrected = 0.0f;
    if (corrected > 100.0f) corrected = 100.0f;
    return corrected;
}

extern "C" uint32_t soc_hybrid_arena_size(void) { return hyb::kArena; }
