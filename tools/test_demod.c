/*
 * Test the headless JAERO demod with recorded audio.
 * Usage: test_demod <input.raw> [baud] [sample_rate]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../jaero_dsp/jaero_demod.h"

/* test_demod doesn't link main.c; supply the globals that jaero_demod.cpp
 * expects via extern. */
double oqpsk_lockingbw = 0;

static int acars_count = 0;
static int bits_count = 0;

static void bits_cb(const unsigned char *bits, int num_bits, int channel_id, void *user)
{
    (void)user; (void)channel_id;
    bits_count += num_bits;
}

static void acars_cb(const uint8_t *data, int len, int channel_id,
                     uint32_t aes_id, uint8_t ges_id,
                     uint8_t qno, uint8_t refno, int downlink, void *user)
{
    (void)user; (void)aes_id; (void)ges_id; (void)qno; (void)refno; (void)downlink;
    acars_count++;
    fprintf(stderr, "[DECODED ch%d] %d bytes:", channel_id, len);
    for (int i = 0; i < len && i < 80; i++) {
        if (data[i] >= 0x20 && data[i] < 0x7f)
            fputc(data[i], stderr);
        else
            fprintf(stderr, "\\x%02X", data[i]);
    }
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.raw> [baud] [sample_rate]\n", argv[0]);
        return 1;
    }

    double baud = argc > 2 ? atof(argv[2]) : 600.0;
    double sample_rate = argc > 3 ? atof(argv[3]) : 48000.0;

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror("fopen"); return 1; }

    fprintf(stderr, "Testing demod: baud=%.0f rate=%.0f\n", baud, sample_rate);

    /* Override freq_center and lockingbw via environment for testing */
    jaero_msk_demod_t *msk = jaero_msk_create(sample_rate, baud, 0, bits_cb, NULL);
    jaero_msk_set_acars_callback(msk, acars_cb, NULL);

    int16_t buf[4096];
    int total = 0;
    while (!feof(fp)) {
        int n = fread(buf, sizeof(int16_t), 4096, fp);
        if (n <= 0) break;
        jaero_msk_feed(msk, buf, n);
        total += n;
    }

    fclose(fp);
    fprintf(stderr, "Fed %d samples (%.1f sec). Bits: %d, ACARS: %d\n",
            total, total / sample_rate, bits_count, acars_count);

    jaero_msk_destroy(msk);
    return 0;
}
