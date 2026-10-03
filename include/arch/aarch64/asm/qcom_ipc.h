/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* SMP2P, GLINK and QRTR to a Qualcomm phone's modem (kernel/arch/aarch64/qcom_ipc.c). */
#ifndef ASM_QCOM_IPC_H
#define ASM_QCOM_IPC_H
#include "types.h"

int     qcom_smp2p_init(void);                  /* our SMP2P item, before the modem starts */
int64_t qcom_smp2p_in(const char *name);        /* the modem's entry, -1 if none */
void    qcom_ipc_start(void);                   /* the polling thread: GLINK, QRTR */
void    qcom_ipc_init(void);                    /* /dev/qrtr */

/* A QRTR server here: `handler` gets each packet to its port. */
typedef void (*qrtr_handler_t)(uint32_t node, uint32_t port, const uint8_t *data, size_t len);
int qrtr_add_server(uint32_t service, uint32_t version, uint32_t instance, uint32_t port, qrtr_handler_t handler);
int qrtr_sendto(uint32_t src_port, uint32_t node, uint32_t port, const void *data, size_t len);
int qrtr_lookup(uint32_t service, uint32_t *node, uint32_t *port);
int qrtr_bind_port(uint32_t port, qrtr_handler_t handler);  /* a port that is not a server */
void qrtr_unbind_port(uint32_t port);
uint32_t qrtr_rx_port(void);                   /* in a handler: the port the packet came to */
void qcom_tftp_setup(void);                     /* qcom_tftp.c: the modem's files */
void qcom_wlan_poll(void);                      /* qcom_wlan.c: WLFW, from the IPC thread */
size_t qcom_wlan_report(char *buf, size_t len);
size_t qcom_tftp_report(char *buf, size_t len);
void qcom_pdmap_setup(void);                    /* qcom_pdmap.c: the protection domain mapper */
size_t qcom_pdmap_report(char *buf, size_t len);
int qcom_mss_start(void);                       /* qcom_mss.c */
const char *qcom_mss_state(void);
void qcom_wlan_init(void);                      /* /dev/wlan */
int qcom_pmic_read(unsigned sid, uint16_t addr, uint8_t *out);  /* qcom_keys.c */
/* qcom_ath.c: the WLAN's copy engines, HTC, WMI */
int    qcom_ath_start(void);
void   qcom_ath_poll(void);
int    qcom_ath_scan(void);
int    qcom_ath_state(void);                    /* 0 not up, 1 up, 2 scanning, -1 failed */
size_t qcom_ath_nets(char *buf, size_t len);
size_t qcom_ath_report(char *buf, size_t len);

int    qcom_rmtfs_setup(void);                  /* the modem's file system service (qcom_rmtfs.c) */
size_t qcom_rmtfs_report(char *buf, size_t len);

#endif
