/*
 * SOH regressor inference. Model: Dense(32->32->16->8->1) with ReLU, linear
 * output. Output range [-1, +1] denormalizes to capacity in Ah using
 * CAP_MIN/CAP_MAX from train.py.
 */

#include "tflm_soh.h"
#include "model.hpp"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <math.h>
#include <stdint.h>

namespace {

/* Must match CAP_MIN / CAP_MAX in train.py. */
constexpr float CAP_MIN = 1.0f;
constexpr float CAP_MAX = 2.0f;

constexpr int kArenaSize = 4 * 1024;
alignas(16) uint8_t g_arena[kArenaSize];

const tflite::Model       *s_model       = nullptr;
tflite::MicroInterpreter  *s_interpreter = nullptr;
TfLiteTensor              *s_input       = nullptr;
TfLiteTensor              *s_output      = nullptr;
bool                       s_ready       = false;

}

extern "C" int tflm_soh_init(void)
{
	s_model = tflite::GetModel(::g_model);
	if (s_model->version() != TFLITE_SCHEMA_VERSION) {
		MicroPrintf("model schema mismatch");
		return -1;
	}

	static tflite::MicroMutableOpResolver<3> resolver;
	resolver.AddFullyConnected();
	resolver.AddRelu();
	resolver.AddReshape();

	static tflite::MicroInterpreter interpreter(
		s_model, resolver, g_arena, kArenaSize);
	s_interpreter = &interpreter;

	if (s_interpreter->AllocateTensors() != kTfLiteOk) {
		MicroPrintf("AllocateTensors failed");
		return -2;
	}

	s_input  = s_interpreter->input(0);
	s_output = s_interpreter->output(0);

	s_ready = true;
	return 0;
}

extern "C" int tflm_soh_estimate(const float *window, float *cap_out)
{
	if (!s_ready || !window || !cap_out) {
		return -1;
	}

	const float in_scale  = s_input->params.scale;
	const int   in_zp     = s_input->params.zero_point;

	for (int i = 0; i < TFLM_SOH_WINDOW; ++i) {
		int32_t q = (int32_t)lroundf(window[i] / in_scale) + in_zp;
		if (q < -128) { q = -128; }
		if (q >  127) { q =  127; }
		s_input->data.int8[i] = (int8_t)q;
	}

	if (s_interpreter->Invoke() != kTfLiteOk) {
		return -2;
	}

	const float out_scale = s_output->params.scale;
	const int   out_zp    = s_output->params.zero_point;
	float y_norm = (s_output->data.int8[0] - out_zp) * out_scale;
	/* Clamp: occasionally the int8 model overshoots [-1,+1] slightly. */
	if (y_norm < -1.0f) { y_norm = -1.0f; }
	if (y_norm >  1.0f) { y_norm =  1.0f; }
	*cap_out = (y_norm + 1.0f) * 0.5f * (CAP_MAX - CAP_MIN) + CAP_MIN;
	return 0;
}

extern "C" uint32_t tflm_soh_arena_size(void)
{
	return (uint32_t)kArenaSize;
}
