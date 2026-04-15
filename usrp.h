/*
 * USRP (UHD) native backend for inmarsat-sniffer
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef __INMARSAT_USRP_H__
#define __INMARSAT_USRP_H__

void usrp_backend_list(void);
void *usrp_backend_setup(const char *serial);
void *usrp_stream_thread(void *arg);
char *usrp_get_serial(const char *name);

#endif
