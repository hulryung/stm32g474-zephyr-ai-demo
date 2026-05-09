#include "signal_gen.h"

#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

#define WINDOW TFLM_ANOMALY_WINDOW

static enum injection_kind s_kind = INJECTION_NONE;
static float               s_amp  = 0.0f;
static struct k_mutex      s_lock;

void signal_gen_init(void)
{
	k_mutex_init(&s_lock);
	srand((unsigned)k_uptime_get());
}

void signal_gen_set_injection(enum injection_kind kind, float amp)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	s_kind = kind;
	s_amp  = amp;
	k_mutex_unlock(&s_lock);
}

void signal_gen_get_injection(enum injection_kind *kind, float *amp)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	if (kind) { *kind = s_kind; }
	if (amp)  { *amp  = s_amp;  }
	k_mutex_unlock(&s_lock);
}

static float frand_pm1(void)
{
	/* uniform in [-1, 1] */
	return ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
}

void signal_gen_next(float *out)
{
	/* Match train.py: sin(2π·1·t + φ1) + 0.3·sin(2π·3·t + φ2) + 0.05·noise
	 * Phase is randomized per call so the AE sees varied normal windows.
	 */
	float phi1 = frand_pm1() * 3.14159265f;
	float phi2 = frand_pm1() * 3.14159265f;
	const float two_pi = 6.28318530f;

	enum injection_kind kind;
	float amp;
	signal_gen_get_injection(&kind, &amp);

	for (int i = 0; i < WINDOW; ++i) {
		float t = (float)i / (float)WINDOW;
		float v = sinf(two_pi * 1.0f * t + phi1)
		        + 0.3f * sinf(two_pi * 3.0f * t + phi2)
		        + 0.05f * frand_pm1();

		switch (kind) {
		case INJECTION_PULSE:
			if (i >= WINDOW / 2 - 1 && i <= WINDOW / 2 + 1) {
				v += amp;
			}
			break;
		case INJECTION_DRIFT:
			v += amp;
			break;
		case INJECTION_NOISE:
			v += amp * frand_pm1();
			break;
		case INJECTION_NONE:
		default:
			break;
		}

		out[i] = v;
	}
}
