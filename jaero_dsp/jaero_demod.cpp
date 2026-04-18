/*
 * jaero_demod.cpp -- C API wrapper for JAERO burst demodulators
 *
 * DSP code originally from JAERO by Jonathan Olds, MIT license.
 */
#include "jaero_demod.h"
#include "burstmskdemodulator.h"
#include "burstoqpskdemodulator.h"
#include "mskdemodulator.h"
#include "oqpskdemodulator.h"
#include "aerol.h"

#define JAERO_BLOCK_SIZE 8192  /* large enough for JFastFir's overlap-save FFT */

struct jaero_msk_demod {
    BurstMskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
    int16_t buf[JAERO_BLOCK_SIZE];
    int buf_len;
};

struct jaero_oqpsk_demod {
    BurstOqpskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
};

/* internal: forward decoded ACARS data from AeroL to the C callback */
static void aerol_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_msk_demod_t *d = (jaero_msk_demod_t *)ctx;
    if (d->acars_cb && acarsitem.valid) {
        /* pack ISU userdata as the output payload */
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id, d->acars_user);
    }
}

/* internal callback adapters */
static void msk_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_msk_demod_t *d = (jaero_msk_demod_t *)ctx;

    /* feed soft bits through AeroL for frame decoding */
    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }

    if (d->cb) {
        /* soft bits are stored as unsigned char values in short array */
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

/* OQPSK -> AeroL ACARS adapter. */
static void oqpsk_aerol_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_oqpsk_demod_t *d = (jaero_oqpsk_demod_t *)ctx;
    if (d->acars_cb && acarsitem.valid) {
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id, d->acars_user);
    }
}

static void oqpsk_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_oqpsk_demod_t *d = (jaero_oqpsk_demod_t *)ctx;

    /* Feed AeroL for full frame decode (burst mode = true for OQPSK). */
    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }

    if (d->cb) {
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

extern "C" {

jaero_msk_demod_t *jaero_msk_create(double sample_rate, double symbol_rate, int channel_id,
                                     jaero_soft_bits_cb cb, void *user)
{
    jaero_msk_demod_t *d = new jaero_msk_demod_t;
    d->channel_id = channel_id;
    d->cb = cb;
    d->user = user;
    d->acars_cb = NULL;
    d->acars_user = NULL;
    d->demod = new BurstMskDemodulator();

    /* Create AeroL in CONTINUOUS (P-channel) mode. Inmarsat Aero P-channel
     * is a continuous framed signal (sync+header+data) not bursts.
     * Prior sessions had this as true (burst mode) which is wrong for P-channel. */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);

    BurstMskDemodulator::Settings s;
    s.Fs = sample_rate;
    s.fb = symbol_rate;
    s.freq_center = 0.0;     /* baseband IQ from channelizer */
    s.lockingbw = 2000.0;    /* ±1000 Hz for SDR PPM offset */
    s.coarsefreqest_fft_power = 13;
    s.symbolspercycle = (symbol_rate <= 600) ? 8 : 16;
    s.signalthreshold = 0.6;

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(msk_bits_adapter, d);
    d->buf_len = 0;

    return d;
}

void jaero_msk_feed(jaero_msk_demod_t *d, const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    for (int i = 0; i < num_samples; i++) {
        d->buf[d->buf_len++] = audio[i];
        if (d->buf_len >= JAERO_BLOCK_SIZE) {
            d->demod->feedAudio(d->buf, d->buf_len, 0);
            d->buf_len = 0;
        }
    }
}

void jaero_msk_feed_soft_bits(jaero_msk_demod_t *d, const short *bits, int num_bits)
{
    if (!d || !d->aerol) return;
    std::vector<short> sv(bits, bits + num_bits);
    d->aerol->processDemodulatedSoftBits(sv);
}

void jaero_msk_feed_iq(jaero_msk_demod_t *d, const double *iq_interleaved, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedIQ(iq_interleaved, num_samples);
}

void jaero_msk_destroy(jaero_msk_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_msk_set_acars_callback(jaero_msk_demod_t *d, jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb = cb;
    d->acars_user = user;
    if (d->aerol) {
        d->aerol->setACARSCallback(aerol_acars_adapter, d);
    }
}

jaero_msk_demod_t *jaero_aerol_create(double symbol_rate, int channel_id,
                                       jaero_acars_cb acars_cb, void *user)
{
    jaero_msk_demod_t *d = new jaero_msk_demod_t;
    d->channel_id = channel_id;
    d->cb = NULL;
    d->user = NULL;
    d->acars_cb = acars_cb;
    d->acars_user = user;
    d->demod = NULL;
    d->buf_len = 0;

    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* P-channel continuous mode */
    if (acars_cb)
        d->aerol->setACARSCallback(aerol_acars_adapter, d);

    return d;
}

jaero_oqpsk_demod_t *jaero_oqpsk_create(double sample_rate, double symbol_rate, int channel_id,
                                          jaero_soft_bits_cb cb, void *user)
{
    jaero_oqpsk_demod_t *d = new jaero_oqpsk_demod_t;
    d->channel_id = channel_id;
    d->cb = cb;
    d->user = user;
    d->acars_cb = NULL;
    d->acars_user = NULL;
    d->demod = new BurstOqpskDemodulator();

    /* AeroL: 10500 forward link is continuous (not burst), same as
     * P-channel MSK. JAERO GUI confirms "10500" (continuous) decodes,
     * "10500 burst" does not on L-band forward link. */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* continuous mode */

    BurstOqpskDemodulator::Settings s;
    s.Fs = sample_rate;
    s.fb = symbol_rate;
    /* JAERO desktop defaults: 8 kHz audio center, 10.5 kHz locking BW.
     * Matches AUDIO_CENTER_HZ in main.c — if one changes, update both.
     * signalthreshold lowered from JAERO default (0.6) to give weaker
     * L-band forward-link C-channel bursts a chance to sync. */
    s.freq_center = 8000.0;
    s.lockingbw = 10500.0;
    s.coarsefreqest_fft_power = 13;
    s.signalthreshold = 0.3;
    s.channel_stereo = false;

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(oqpsk_bits_adapter, d);
    d->demod->setAFC(true);  /* on by default in JAERO GUI */

    return d;
}

void jaero_oqpsk_feed(jaero_oqpsk_demod_t *d, const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedAudio(audio, num_samples, 0);
}

void jaero_oqpsk_destroy(jaero_oqpsk_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_oqpsk_set_acars_callback(jaero_oqpsk_demod_t *d,
                                     jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb = cb;
    d->acars_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(oqpsk_aerol_acars_adapter, d);
}

/* ============================================================
 * Continuous MSK demodulator (P-channel)
 * ============================================================ */

struct jaero_pmsk_demod {
    MskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
};

static void pmsk_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }
    if (d->cb) {
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

static void pmsk_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_pmsk_demod_t *d = (jaero_pmsk_demod_t *)ctx;
    if (d->acars_cb && acarsitem.valid) {
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id, d->acars_user);
    }
}

jaero_pmsk_demod_t *jaero_pmsk_create(double sample_rate, double symbol_rate,
                                       int channel_id,
                                       jaero_soft_bits_cb cb, void *user)
{
    jaero_pmsk_demod_t *d = new jaero_pmsk_demod_t;
    d->channel_id = channel_id;
    d->cb = cb;
    d->user = user;
    d->acars_cb = NULL;
    d->acars_user = NULL;
    d->demod = new MskDemodulator();

    /* AeroL in CONTINUOUS (P-channel) mode */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* burstmode=false */

    MskDemodulator::Settings s;
    s.Fs = sample_rate;
    s.fb = symbol_rate;
    /* Our feedIQ wrapper mixes baseband IQ → int16 audio at freq_center Hz
     * (same formula as our ZMQ out → real JAERO, known to decode).
     * Demod's mixer_center brings audio back to baseband. */
    s.freq_center = 1000.0;
    s.lockingbw = 900.0;
    s.coarsefreqest_fft_power = 13;
    s.symbolspercycle = (symbol_rate <= 600) ? 8 : 16;
    s.signalthreshold = 0.5;

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(pmsk_bits_adapter, d);
    d->demod->setAFC(true);  /* on by default in JAERO GUI */

    return d;
}

void jaero_pmsk_feed_iq(jaero_pmsk_demod_t *d,
                         const double *iq_interleaved, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedIQ(iq_interleaved, num_samples);
}

void jaero_pmsk_feed_audio(jaero_pmsk_demod_t *d,
                            const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedAudio(audio, num_samples, 0);
}

void jaero_pmsk_destroy(jaero_pmsk_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_pmsk_set_acars_callback(jaero_pmsk_demod_t *d,
                                     jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb = cb;
    d->acars_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(pmsk_acars_adapter, d);
}

/* ============================================================
 * Continuous OQPSK demodulator (Aero H/H+/L, 10500 baud forward link)
 * Uses OqpskDemodulator (not BurstOqpskDemodulator).
 * AeroL in continuous (non-burst) mode — same as P-channel MSK.
 * ============================================================ */

struct jaero_oqpsk_cont_demod {
    OqpskDemodulator *demod;
    AeroL *aerol;
    int channel_id;
    jaero_soft_bits_cb cb;
    void *user;
    jaero_acars_cb acars_cb;
    void *acars_user;
};

static void oqpsk_cont_aerol_acars_adapter(ACARSItem &acarsitem, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;
    if (d->acars_cb && acarsitem.valid) {
        const uint8_t *data = acarsitem.isuitem.userdata.data();
        int len = (int)acarsitem.isuitem.userdata.size();
        d->acars_cb(data, len, d->channel_id, d->acars_user);
    }
}

static void oqpsk_cont_bits_adapter(const short *bits, int num_bits, void *ctx)
{
    jaero_oqpsk_cont_demod_t *d = (jaero_oqpsk_cont_demod_t *)ctx;

    if (d->aerol) {
        std::vector<short> sv(bits, bits + num_bits);
        d->aerol->processDemodulatedSoftBits(sv);
    }

    if (d->cb) {
        std::vector<unsigned char> buf(num_bits);
        for (int i = 0; i < num_bits; i++)
            buf[i] = (unsigned char)(bits[i] & 0xFF);
        d->cb(buf.data(), num_bits, d->channel_id, d->user);
    }
}

jaero_oqpsk_cont_demod_t *jaero_oqpsk_cont_create(double sample_rate, double symbol_rate,
                                                    int channel_id,
                                                    jaero_soft_bits_cb cb, void *user)
{
    jaero_oqpsk_cont_demod_t *d = new jaero_oqpsk_cont_demod_t;
    d->channel_id  = channel_id;
    d->cb          = cb;
    d->user        = user;
    d->acars_cb    = NULL;
    d->acars_user  = NULL;
    d->demod       = new OqpskDemodulator();

    /* AeroL: continuous (non-burst) mode — 10500 baud forward link is
     * continuous, not burst. Same as P-channel MSK. */
    d->aerol = new AeroL();
    d->aerol->setSettings(symbol_rate, false);  /* burstmode=false */

    OqpskDemodulator::Settings s;
    s.Fs          = sample_rate;
    s.fb          = symbol_rate;
    /* JAERO desktop defaults for 10500 baud: 8 kHz audio center,
     * 10.5 kHz locking bandwidth. feedAudio path expects signal at freq_center.
     * feedIQ wrapper mixes IQ → audio at freq_center (same as ZMQ output). */
    s.freq_center              = 8000.0;
    s.lockingbw                = 10500.0;
    s.coarsefreqest_fft_power  = 14;
    s.signalthreshold          = 0.3;  /* lowered for weak L-band forward link */

    d->demod->setSettings(s);
    d->demod->setSoftBitsCallback(oqpsk_cont_bits_adapter, d);
    d->demod->setAFC(true);  /* on by default in JAERO GUI */

    return d;
}

void jaero_oqpsk_cont_feed_audio(jaero_oqpsk_cont_demod_t *d,
                                   const int16_t *audio, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedAudio(audio, num_samples, 0);
}

void jaero_oqpsk_cont_feed_iq(jaero_oqpsk_cont_demod_t *d,
                                const double *iq_interleaved, int num_samples)
{
    if (!d || !d->demod) return;
    d->demod->feedIQ(iq_interleaved, num_samples);
}

void jaero_oqpsk_cont_destroy(jaero_oqpsk_cont_demod_t *d)
{
    if (!d) return;
    delete d->aerol;
    delete d->demod;
    delete d;
}

void jaero_oqpsk_cont_set_acars_callback(jaero_oqpsk_cont_demod_t *d,
                                          jaero_acars_cb cb, void *user)
{
    if (!d) return;
    d->acars_cb   = cb;
    d->acars_user = user;
    if (d->aerol)
        d->aerol->setACARSCallback(oqpsk_cont_aerol_acars_adapter, d);
}

} /* extern "C" */
