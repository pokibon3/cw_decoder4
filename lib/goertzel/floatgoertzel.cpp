#include <math.h>
#include <stdint.h>

static float coeff_f = 1.73f;

void setSpeed(int16_t speed)
{
    switch (speed) {
        case 0:
            coeff_f = 1.73f;
            break;
        case 1:
            coeff_f = 1.58f;
            break;
        case 2:
            coeff_f = 1.41f;
            break;
        default:
            coeff_f = 1.73f;
            break;
    }
}

void initGoertzel(int16_t speed)
{
    setSpeed(speed);
}

int32_t goertzel(int16_t *data, int16_t n)
{
    float q0 = 0.0f;
    float q1 = 0.0f;
    float q2 = 0.0f;

    for (int16_t i = 0; i < n; i++) {
        q0 = coeff_f * q1 - q2 + (float)data[i];
        q2 = q1;
        q1 = q0;
    }

    float mag2 = (q1 * q1) + (q2 * q2) - (q1 * q2 * coeff_f);
    if (mag2 < 0.0f) {
        mag2 = 0.0f;
    }
    return (int32_t)(sqrtf(mag2) + 0.5f);
}
