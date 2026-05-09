/*
 * Autoencoder inference + reconstruction-error scoring.
 * Model architecture (from train.py): Dense(16→8→4→8→16→32) with ReLU
 * activations; int8-quantized I/O.
 */

#include "tflm_anomaly.h"
#include "model.hpp"

#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/micro_log.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

#include <math.h>
#include <stdint.h>

namespace {

constexpr int kArenaSize = 4 * 1024;       /* generous for a tiny dense model */
alignas(16) uint8_t g_arena[kArenaSize];

const tflite::Model       *s_model       = nullptr;
tflite::MicroInterpreter  *s_interpreter = nullptr;
TfLiteTensor              *s_input       = nullptr;
TfLiteTensor              *s_output      = nullptr;
bool                       s_ready       = false;

}  /* namespace */

extern "C" int tflm_anomaly_init(void)
{
	s_model = tflite::GetModel(::g_model);
	if (s_model->version() != TFLITE_SCHEMA_VERSION) {
		MicroPrintf("model schema %d != supported %d",
			    s_model->version(), TFLITE_SCHEMA_VERSION);
		return -1;
	}

	/* Dense+ReLU networks need FullyConnected + Relu; Keras sometimes
	 * also emits a separate Reshape so we add it just in case.
	 */
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

	if (s_input->dims->data[s_input->dims->size - 1] != TFLM_ANOMALY_WINDOW ||
	    s_output->dims->data[s_output->dims->size - 1] != TFLM_ANOMALY_WINDOW) {
		MicroPrintf("unexpected tensor shape (input/output last dim)");
		return -3;
	}

	s_ready = true;
	return 0;
}

extern "C" int tflm_anomaly_score(const float *window, float *out_score)
{
	if (!s_ready || window == nullptr || out_score == nullptr) {
		return -1;
	}

	const float in_scale = s_input->params.scale;
	const int   in_zp    = s_input->params.zero_point;

	/* float -> int8 quantize into input tensor */
	for (int i = 0; i < TFLM_ANOMALY_WINDOW; ++i) {
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

	/* int8 -> float dequantize and accumulate squared error vs the input */
	float sse = 0.0f;
	for (int i = 0; i < TFLM_ANOMALY_WINDOW; ++i) {
		float y = (s_output->data.int8[i] - out_zp) * out_scale;
		float diff = window[i] - y;
		sse += diff * diff;
	}
	*out_score = sse / (float)TFLM_ANOMALY_WINDOW;
	return 0;
}

extern "C" uint32_t tflm_anomaly_arena_size(void)
{
	return (uint32_t)kArenaSize;
}
