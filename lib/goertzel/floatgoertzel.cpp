//
//	3-bin float Goertzel filter
//
//	Center bin tracks the target tone; two side bins sit exactly
//	+-2 DFT bins away (+-341.33 Hz @ 8192 Hz / 48 samples), on nulls of
//	the rectangular-window Dirichlet kernel. A clean tone therefore
//	leaks almost nothing into the side bins, while white noise raises
//	all three bins roughly equally - the center/side ratio separates
//	tone from noise.
//
//	Two side levels are reported:
//	- goertzelSideMag()     = min(EMA(low), EMA(high)). min() keeps a
//	  one-sided interferer (QRM) from masking the tone; the per-side
//	  EMA (alpha = 1/4, ~4 blocks = 23 ms) removes the block-to-block
//	  variance of white noise, which would otherwise make min() of two
//	  noisy bins dip low and pass as "tone".
//	- goertzelSideMagInst() = min(low, high) of the current block only.
//	  A broadband impulse raises the side bins instantly, but the EMA
//	  lags ~4 blocks, so the first burst block would slip through the
//	  smoothed gate. The decoder checks the instantaneous level too
//	  when it is about to turn ON from silence.
//
#include <math.h>
#include <stdint.h>

// coeff = 2 * cos(2 * pi * f / 8192)
static const float coeff_tbl[3][3] = {
	// center      low(-341.3Hz) high(+341.3Hz)
	{ 1.744167f, 1.938046f, 1.431426f },  // 666.7 Hz (sides 325.4 / 1008.0)
	{ 1.605226f, 1.859301f, 1.241759f },  // 833.3 Hz (sides 492.0 / 1174.6)
	{ 1.440005f, 1.750164f, 1.031712f },  // 1000 Hz (sides 658.7 / 1341.3)
};

static float coeff_c = 1.744167f;
static float coeff_l = 1.938046f;
static float coeff_h = 1.431426f;

static int32_t side_mag = 0;
static int32_t side_mag_inst = 0;
static int32_t side_ema_l = 0;
static int32_t side_ema_h = 0;
static uint8_t side_ema_started = 0;

void setSpeed(int16_t speed)
{
    if (speed < 0 || speed > 2) {
        speed = 0;
    }
    coeff_c = coeff_tbl[speed][0];
    coeff_l = coeff_tbl[speed][1];
    coeff_h = coeff_tbl[speed][2];
}

void initGoertzel(int16_t speed)
{
    setSpeed(speed);
    side_mag = 0;
    side_mag_inst = 0;
    side_ema_l = 0;
    side_ema_h = 0;
    side_ema_started = 0;
}

static inline int32_t goertzel_mag(float q1, float q2, float coeff)
{
    float mag2 = (q1 * q1) + (q2 * q2) - (q1 * q2 * coeff);
    if (mag2 < 0.0f) {
        mag2 = 0.0f;
    }
    return (int32_t)(sqrtf(mag2) + 0.5f);
}

int32_t goertzel(int16_t *data, int16_t n)
{
    float q1c = 0.0f, q2c = 0.0f;
    float q1l = 0.0f, q2l = 0.0f;
    float q1h = 0.0f, q2h = 0.0f;

    for (int16_t i = 0; i < n; i++) {
        const float x = (float)data[i];
        float q0;
        q0 = coeff_c * q1c - q2c + x; q2c = q1c; q1c = q0;
        q0 = coeff_l * q1l - q2l + x; q2l = q1l; q1l = q0;
        q0 = coeff_h * q1h - q2h + x; q2h = q1h; q1h = q0;
    }

    const int32_t mag_l = goertzel_mag(q1l, q2l, coeff_l);
    const int32_t mag_h = goertzel_mag(q1h, q2h, coeff_h);

    side_mag_inst = (mag_l < mag_h) ? mag_l : mag_h;

    if (!side_ema_started) {
        side_ema_started = 1;
        side_ema_l = mag_l;
        side_ema_h = mag_h;
    } else {
        side_ema_l += (mag_l - side_ema_l) / 4;
        side_ema_h += (mag_h - side_ema_h) / 4;
    }
    side_mag = (side_ema_l < side_ema_h) ? side_ema_l : side_ema_h;

    return goertzel_mag(q1c, q2c, coeff_c);
}

// Smoothed min side-bin magnitude as of the last goertzel() call.
int32_t goertzelSideMag(void)
{
    return side_mag;
}

// Instantaneous (unsmoothed) min side-bin magnitude of the last block.
int32_t goertzelSideMagInst(void)
{
    return side_mag_inst;
}
