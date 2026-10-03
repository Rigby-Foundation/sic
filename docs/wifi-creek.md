# Wi-Fi on a "creek" phone (Redmi 15 4G, SM6115-class, WCN3990-family)

The WLAN radio is not a PCIe or SDIO card that the apps CPU drives by itself.
Its firmware runs inside the modem (MPSS). Linux's `ath10k_snoc` model
applies: the apps CPU only talks to the copy engines (CE) at `qcom,wcn3990`,
and the modem has to be booted and serving first. These are the layers, from
the bottom up, and where sic stands on each:

| layer | what it is | status |
|---|---|---|
| SMEM | shared heap at `smem_region` (0x46000000, 2 MB) | **read-only parser, `/dev/smem`** |
| modem PAS boot | `qcom,bengal-modem-pas` @ 0x6080000: load `modem.mdt` + `.bNN` (from the modem partition) into `modem_region` 0x4ab00000, then TrustZone `PAS_INIT_IMAGE/AUTH_AND_RESET` SCM calls | todo (the zap shader loader in `qcom_gpu.c` already does a PAS for the GPU) |
| SMP2P | modem ↔ apps state bits (items 0x1b3/0x1ac, IRQ SPI 0x46, mailbox bit 14): fatal/ready/handover/stop-ack | todo |
| GLINK over SMEM | FIFOs in SMEM, IRQ SPI 0x44, mailbox bit 12 on `apcs_glb` 0x0f111000; channel `IPCRTR` | todo |
| QRTR + name service | the modem's services are found by name | todo |
| QMI services the modem needs | rmtfs (modemst1/2, fsg partitions), the tftp server it fetches its configs from | todo |
| WLFW (QMI) | send the board data (`bdwlan.*`), cal, mode ON | todo |
| ath10k CE/HTC/WMI/HTT | copy engines behind the apps SMMU, `wlan_msa` 0x51900000 | todo |
| 802.11 + WPA2 | scan, auth/assoc, 4-way handshake (a supplicant in user space) | todo |
| net device | a `wlan0` for kernel/net | todo |

Each layer can be tested on the phone only once the one below works, and
the modem also needs the remote file system (rmtfs) before it brings the
radio up. That is why this goes one layer at a time.
