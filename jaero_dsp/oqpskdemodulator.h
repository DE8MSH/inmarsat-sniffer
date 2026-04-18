/*
 * OqpskDemodulator -- JAERO's CONTINUOUS OQPSK demodulator for Aero H/H+/L channels.
 * Ported from https://github.com/jontio/JAERO, Qt stripped to pure C++.
 * SPDX-License-Identifier: MIT (JAERO original license)
 */
#ifndef OQPSKDEMODULATOR_H
#define OQPSKDEMODULATOR_H

#include "DSP.h"
#include "coarsefreqestimate.h"
#include <vector>
#include <complex>
#include <cstdint>

class CoarseFreqEstimate;

/* Callback: soft bits ready. Soft bit is unsigned char 0-255, 128 = threshold. */
typedef void (*oqpsk_soft_bits_cb)(const short *bits, int num_bits, void *user);

class OqpskDemodulator
{
public:
    enum ScatterPointType { SPT_constellation, SPT_phaseoffseterror,
                            SPT_phaseoffsetest, SPT_None };
    struct Settings
    {
        int    coarsefreqest_fft_power;
        double freq_center;
        double lockingbw;
        double fb;
        double Fs;
        double signalthreshold;
        Settings()
        {
            coarsefreqest_fft_power = 14;
            freq_center             = 8000;   /* Hz */
            lockingbw               = 10500;  /* Hz */
            fb                      = 10500;  /* bps */
            Fs                      = 48000;  /* Hz */
            signalthreshold         = 0.65;
        }
    };

    OqpskDemodulator();
    ~OqpskDemodulator();

    void setSettings(Settings settings);
    void invalidatesettings();
    void setAFC(bool state);
    void setSQL(bool state);
    void setCPUReduce(bool state);
    void setScatterPointType(ScatterPointType type);
    double getCurrentFreq();

    /* Feed int16 mono audio (signal centered at freq_center Hz). */
    void feedAudio(const int16_t *samples, int num_samples, int sample_rate);
    /* Feed complex baseband IQ (interleaved re,im doubles). */
    void feedIQ(const double *iq_interleaved, int num_samples);

    /* Callback registration. */
    void setSoftBitsCallback(oqpsk_soft_bits_cb cb, void *user);

private:
    oqpsk_soft_bits_cb soft_bits_cb;
    void *soft_bits_user;

    /* Internal slot equivalents (called directly, not via Qt). */
    void FreqOffsetEstimateSlot(double freq_offset_est);
    void DCDstatSlot(bool _dcd);
    void processAudio(const short *data, int num_samples);

    bool afc;
    bool sql;
    int  scatterpointtype;

    std::vector<double>    spectrumcycbuff;
    int spectrumcycbuff_ptr;
    int spectrumnfft;

    std::vector<cpx_type>  bbcycbuff;
    std::vector<cpx_type>  bbtmpbuff;
    int bbcycbuff_ptr;
    int bbnfft;

    std::vector<cpx_type>  pointbuff;
    int pointbuff_ptr;

    double Fs;
    double freq_center;
    double lockingbw;
    double fb;
    double signalthreshold;

    double SamplesPerSymbol;

    FIR *fir_re;
    FIR *fir_im;

    /* Symbol timing */
    Delay<double> delays;
    Delay<double> delayt41;
    Delay<double> delayt42;
    Delay<double> delayt8;
    IIR  st_iir_resonator;
    WaveTable st_osc;
    WaveTable st_osc_ref;

    /* Carrier tracking loop filter */
    IIR  ct_iir_loopfilter;

    WaveTable mixer_center;
    WaveTable mixer2;

    CoarseFreqEstimate *coarsefreqestimate;

    double mse;
    MSEcalc *msecalc;

    std::vector<cpx_type>  phasepointbuff;
    int phasepointbuff_ptr;

    AGC *agc;

    OQPSKEbNoMeasure *ebnomeasure;

    BaceConverter bc;
    std::vector<short> RxDataBits;   /* unpacked soft bits */

    MovingAverage *marg;
    DelayThing<cpx_type> dt;

    double ee;

    bool dcd;

    /* Pre-filter for 8400 bps path */
    JFastFir fir_pre;
    WaveTable mixer_fir_pre;

    int  coarseCounter;
    bool cpuReduce;

    /* feedIQ mixing state */
    double feediq_phase;

    /* Per-instance state — were static locals in JAERO (single-instance).
     * Must be per-instance for multi-channel parallel demodulation. */
    cpx_type sig2_last;
    int yui;
    cpx_type pt_d;
    int freqest_countdown;
    int freqest_countdown2;
};

#endif /* OQPSKDEMODULATOR_H */
