/*
 * BladeRF native backend for inmarsat-sniffer
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __INMARSAT_BLADERF_H__
#define __INMARSAT_BLADERF_H__

void bladerf_backend_list(void);
void *bladerf_backend_setup(int instance);
void *bladerf_stream_thread(void *arg);

#endif
