/*
 * TFLM-based sine approximator (int8-quantized model from the upstream
 * Zephyr hello_world sample). Exposes a C interface in tflm_sine.h.
 */

#include "tflm_sine.h"
#include "model.hpp"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <math.h>

namespace {

constexpr int kArenaSize = 2000;
alignas(16) uint8_t g_arena[kArenaSize];

const tflite::Model        *s_model       = nullptr;
tflite::MicroInterpreter   *g_interpreter = nullptr;
TfLiteTensor               *g_input       = nullptr;
TfLiteTensor               *g_output      = nullptr;
bool                        g_ready       = false;

}  /* namespace */

extern "C" int tflm_sine_init(void)
{
	s_model = tflite::GetModel(::g_model);
	if (s_model->version() != TFLITE_SCHEMA_VERSION) {
		MicroPrintf("model schema %d != supported %d",
			    s_model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}

	static tflite::MicroMutableOpResolver<1> resolver;
	resolver.AddFullyConnected();

	static tflite::MicroInterpreter interpreter(
		s_model, resolver, g_arena, kArenaSize);
	g_interpreter = &interpreter;

	if (g_interpreter->AllocateTensors() != kTfLiteOk) {
		MicroPrintf("AllocateTensors failed");
		return -2;
	}

	g_input  = g_interpreter->input(0);
	g_output = g_interpreter->output(0);
	g_ready  = true;
	return 0;
}

extern "C" int tflm_sine_infer(float x, float *y)
{
	if (!g_ready || g_input == nullptr || g_output == nullptr || y == nullptr) {
		return -1;
	}

	/* Quantize float input -> int8 using tensor's scale + zero-point. */
	int32_t q = (int32_t)lroundf(x / g_input->params.scale)
	             + g_input->params.zero_point;
	if (q < -128) { q = -128; }
	if (q >  127) { q =  127; }
	g_input->data.int8[0] = (int8_t)q;

	if (g_interpreter->Invoke() != kTfLiteOk) {
		return -2;
	}

	int8_t qy = g_output->data.int8[0];
	*y = (qy - g_output->params.zero_point) * g_output->params.scale;
	return 0;
}

extern "C" uint32_t tflm_sine_arena_size(void)
{
	return (uint32_t)kArenaSize;
}
