/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Qualcomm SoC drivers (kernel/drivers/qcom/): each one looks for its
 * hardware in the device tree and stays out of the way when it is not
 * there, so an aarch64 kernel built with them still boots anywhere. */
#pragma once
#include "types.h"

void qcom_init_early(void);         /* before the other CPUs come up: USB serial, so its report survives a failed bring-up */
void qcom_init(void);               /* the rest: buttons, touch, GPU, panel, I2C/battery, SMEM, modem, UFS */
int  qcom_pon_set_reason(uint8_t reason);   /* qcom_keys.c: what the bootloader does after a restart */
