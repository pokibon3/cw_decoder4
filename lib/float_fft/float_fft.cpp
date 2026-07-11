#include "float_fft.h"

#include <math.h>

void float_fft(float *fr, float *fi, int log2n)
{
    const int n = 1 << log2n;

    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t;
            t = fr[i];
            fr[i] = fr[j];
            fr[j] = t;
            t = fi[i];
            fi[i] = fi[j];
            fi[j] = t;
        }
    }

    for (int stage = 1; stage <= log2n; stage++) {
        const int m = 1 << stage;
        const int mh = m >> 1;
        const float ang = -(float)M_PI / (float)mh;
        const float wR = cosf(ang);
        const float wI = sinf(ang);

        for (int k = 0; k < n; k += m) {
            float cR = 1.0f;
            float cI = 0.0f;
            for (int j = 0; j < mh; j++) {
                const float uR = fr[k + j];
                const float uI = fi[k + j];
                const float vR = fr[k + j + mh] * cR - fi[k + j + mh] * cI;
                const float vI = fr[k + j + mh] * cI + fi[k + j + mh] * cR;
                fr[k + j] = uR + vR;
                fi[k + j] = uI + vI;
                fr[k + j + mh] = uR - vR;
                fi[k + j + mh] = uI - vI;

                const float tmp = cR * wR - cI * wI;
                cI = cR * wI + cI * wR;
                cR = tmp;
            }
        }
    }
}

