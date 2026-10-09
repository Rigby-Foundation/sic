/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* The order the Qualcomm drivers come up in (see drivers/qcom.h). */
#include "drivers/qcom.h"

void qcom_usb_probe(void);
void qcom_keys_init(void);
void qcom_touch_init(void);
void qcom_gpu_init(void);
void qcom_panel_init(void);
void qcom_i2c_init(void);
void qcom_smem_init(void);
void qcom_mss_init(void);
void qcom_ufs_init(void);

void qcom_init_early(void)
{
    qcom_usb_probe();
}

void qcom_init(void)
{
    qcom_keys_init();               /* a phone's buttons */
    qcom_touch_init();              /* a "creek" phone's touch controller */
    qcom_gpu_init();                /* its Adreno 610: /dev/adreno, /dev/adrenogpu */
    qcom_panel_init();              /* its panel through the running DSI controller: /dev/panel */
    qcom_i2c_init();                /* its I2C buses and the battery's fuel gauge: /dev/battery */
    qcom_smem_init();               /* its shared memory with the modem (Wi-Fi's way in): /dev/smem */
    qcom_mss_init();                /* its modem, started by hand: /dev/mss */
#ifdef CONFIG_UFS
    qcom_ufs_init();                /* its flash, the bootloader's UFS controller: /dev/sdX */
#endif
}
