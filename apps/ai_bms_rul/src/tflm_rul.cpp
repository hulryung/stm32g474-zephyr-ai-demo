/*
 * RUL regressor inference.
 *
 * Output is a single scalar in [-1, +1] (int8 quantized) which we
 * denormalize to [0, RUL_MAX] cycles. Must mirror RUL_MAX from train.py.
 */

#include "tflm_rul.h"
#include "model.hpp"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <math.h>
#include <stdint.h>

namespace {

constexpr float RUL_MAX = 150.0f;   /* must match train.py */

constexpr int kArenaSize = 4 * 1024;
alignas(16) uint8_t g_arena[kArenaSize];

const tflite::Model       *s_model       = nullptr;
tflite::MicroInterpreter  *s_interpreter = nullptr;
TfLiteTensor              *s_input       = nullptr;
TfLiteTensor              *s_output      = nullptr;
bool                       s_ready       = false;

}

extern "C" int tflm_rul_init(void)
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
	s_ready  = true;
	return 0;
}

extern "C" int tflm_rul_predict(const float *window, int *cycles_out)
{
	if (!s_ready || !window || !cycles_out) {
		return -1;
	}

	const float in_scale = s_input->params.scale;
	const int   in_zp    = s_input->params.zero_point;
	for (int i = 0; i < TFLM_RUL_WINDOW; ++i) {
		int32_t q = (int32_t)lroundf(window[i] / in_scale) + in_zp;
		if (q < -128) { q = -128; }
		if (q >  127) { q =  127; }
		s_input->data.int8[i] = (int8_t)q;
	}

	if (s_interpreter->Invoke() != kTfLiteOk) {
		return -2;
	}

	float y_norm = (s_output->data.int8[0] - s_output->params.zero_point)
	               * s_output->params.scale;
	if (y_norm < -1.0f) { y_norm = -1.0f; }
	if (y_norm >  1.0f) { y_norm =  1.0f; }
	float cycles = (y_norm + 1.0f) * 0.5f * RUL_MAX;
	if (cycles < 0.0f) { cycles = 0.0f; }
	*cycles_out = (int)lroundf(cycles);
	return 0;
}

extern "C" uint32_t tflm_rul_arena_size(void)
{
	return (uint32_t)kArenaSize;
}
