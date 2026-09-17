/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* Special-purpose register numbers and MSR bits (usable from assembly). */
#pragma once

#define SPRN_XER    1
#define SPRN_LR     8
#define SPRN_CTR    9
#define SPRN_DSISR  18
#define SPRN_DAR    19
#define SPRN_DEC    22
#define SPRN_SDR1   25
#define SPRN_SRR0   26
#define SPRN_SRR1   27
#define SPRN_SPRG0  272
#define SPRN_SPRG1  273
#define SPRN_SPRG2  274
#define SPRN_SPRG3  275
#define SPRN_VRSAVE 256
#define SPRN_TBRL   268
#define SPRN_TBRU   269
#define SPRN_PVR    287
#define SPRN_IBAT0U 528
#define SPRN_IBAT0L 529
#define SPRN_IBAT1U 530
#define SPRN_IBAT1L 531
#define SPRN_IBAT2U 532
#define SPRN_IBAT2L 533
#define SPRN_IBAT3U 534
#define SPRN_IBAT3L 535
#define SPRN_DBAT0U 536
#define SPRN_DBAT0L 537
#define SPRN_DBAT1U 538
#define SPRN_DBAT1L 539
#define SPRN_DBAT2U 540
#define SPRN_DBAT2L 541
#define SPRN_DBAT3U 542
#define SPRN_DBAT3L 543
#define SPRN_DBAT4U 568
#define SPRN_DBAT4L 569
#define SPRN_DBAT5U 570
#define SPRN_DBAT5L 571
#define SPRN_DBAT6U 572
#define SPRN_DBAT6L 573
#define SPRN_DBAT7U 574
#define SPRN_DBAT7L 575
#define SPRN_HID0   1008

#define MSR_EE_BIT  15
#define MSR_PR_BIT  14
#define MSR_FP_BIT  13
#define MSR_ME_BIT  12
#define MSR_IR_BIT  5
#define MSR_DR_BIT  4
#define MSR_RI_BIT  1
