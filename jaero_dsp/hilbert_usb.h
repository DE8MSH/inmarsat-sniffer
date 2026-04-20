/*
 * 125-tap Hilbert FIR + 62-sample delay line = USB demod.
 * audio = delay(re) - hilbert(im), matches SDRReceiver's vfo::usb_demod().
 *
 * Used by both MskDemodulator::feedIQ and OqpskDemodulator::feedIQ to get
 * the same analytic-to-real conversion the ZMQ output path produces, which
 * measured ~1.5 dB better Eb/No than the prior `re*cos - im*sin` mixer on
 * our live RTL-SDR capture (ch12).
 */
#ifndef HILBERT_USB_H
#define HILBERT_USB_H

#include <cmath>
#include <vector>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class HilbertUSB {
public:
    static constexpr int TAPS = 125;
    static constexpr int DELAY = (TAPS - 1) / 2;   /* 62 */

    HilbertUSB() { reset(); }

    void reset() {
        design();
        for (int i = 0; i < TAPS; i++) hist[i] = 0.0f;
        for (int i = 0; i < DELAY + 1; i++) dbuf[i] = 0.0f;
        hidx = 0;
        didx = 0;
        bfo_phase = 0.0;
    }

    /* Mix baseband IQ up to freq_center with USB demod, scale to int16.
     * gain is the pre-clip multiplier (matches vfo::usb_demod: audio*gain*32768). */
    void process(const double *iq_interleaved, int n,
                 double Fs, double freq_center, double gain,
                 int16_t *out)
    {
        double phase_inc = 2.0 * M_PI * freq_center / Fs;
        for (int i = 0; i < n; i++) {
            double re = iq_interleaved[i * 2];
            double im = iq_interleaved[i * 2 + 1];

            if (phase_inc != 0.0) {
                double ca = std::cos(bfo_phase);
                double sa = std::sin(bfo_phase);
                double nr = re * ca - im * sa;
                double ni = re * sa + im * ca;
                re = nr; im = ni;
                bfo_phase += phase_inc;
                if (bfo_phase > 2.0 * M_PI) bfo_phase -= 2.0 * M_PI;
            }

            float delayed_re = delay_step((float)re);
            float hilb_im    = hilbert_step((float)im);
            double usb = (double)delayed_re - (double)hilb_im;

            double scaled = usb * gain * 32768.0;
            if (scaled >  32767.0) scaled =  32767.0;
            if (scaled < -32768.0) scaled = -32768.0;
            out[i] = (int16_t)scaled;
        }
    }

private:
    static float *taps_ptr() {
        static float t[TAPS];
        return t;
    }
    static bool& designed() { static bool d = false; return d; }

    float hist[TAPS];
    int   hidx;
    float dbuf[DELAY + 1];
    int   didx;
    double bfo_phase;

    static void design() {
        if (designed()) return;
        float tmp[TAPS];
        float sumsq = 0;
        for (int n = 0; n < TAPS; n++) {
            if (n == TAPS / 2) tmp[n] = 0;
            else {
                double x = M_PI * (n - TAPS / 2);
                tmp[n] = (float)((1.0 - std::cos(x)) / x);
            }
            sumsq += tmp[n] * tmp[n];
        }
        float gain = std::sqrt(sumsq);
        float *t = taps_ptr();
        for (int i = 0; i < TAPS; i++)
            t[i] = tmp[TAPS - i - 1] / gain;
        designed() = true;
    }

    inline float hilbert_step(float im) {
        hist[hidx] = im;
        hidx = (hidx + 1) % TAPS;
        const float *t = taps_ptr();
        float sum = 0;
        int idx = hidx;
        for (int i = 0; i < TAPS; i++) {
            sum += t[i] * hist[idx];
            idx = (idx + 1) % TAPS;
        }
        return sum;
    }

    inline float delay_step(float re) {
        dbuf[didx] = re;
        didx = (didx + 1) % (DELAY + 1);
        return dbuf[didx];
    }
};

#endif
