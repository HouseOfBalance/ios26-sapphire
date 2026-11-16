# Audio (ADSP), Wi-Fi and modem for SM6225: what the iOS 16 port is, and what blocks a drop-in (2026-11-16)

Source: iOS 16 `iokit/Kernel/arm/TopazModem.cpp`, rebuilt from patches 0113-0248 (14,309 lines, one file).
Done so far: the RPM transport (`drivers/power/hakios.rpm.c`, was lines 10916-11306).

## What is in the file
| Lines | Part | Needs |
|---|---|---|
| 92-330 | mappings, TrustZone PAS (SCM calls), firmware files | `sweet_ufs_read_part()` (firmware from UFS partitions), SMC conduit |
| 331-517 | SMEM (0x46000000), TCSR mutex hwlock 3, SMP2P | nothing outside |
| 518-1140 | GLINK over SMEM, QRTR, GLINK RX | `sweet_qmi_deliver/pump/dev_init` |
| 1141-4795 | host services (QMI: rmtfs, PDR, ...) | `sweet_ufs_read_part`, the modem pool |
| 4796-7931 | WLFW (QMI 0x45), WLAN radio (WCN3988, ath10k CE/HTC/WMI/HTT), WPA2, station | `sweet_pdp_*`, `topaz_wlan_bss`, the modem pool |
| 7932-9374 | IPA v4.2 | `sweet_ipa_*`, `sweet_pdp_*` |
| 9375-9527 | PAS boot of the modem (MPSS) | TrustZone, the modem pool |
| 9528-10915 | ADSP boot, its GLINK edge, QRTR, GPR (AudioReach), audio=2 playback | `sweet_audio_*`, `sweet_gpr_*`, `sweet_mic_want`, the pool, SMMU S1 (SID 0x1c1, IOVA 0x10000000) |
| 11308-13005 | codec: RX macro, WCD937x, speaker, microphones | `sweet_i2c1_*` (amp, I2C1), `sweet_spmi_*`, LPI/RX/VA macro MMIO |
| 13006-14309 | FastRPC to the sensors PD, watch thread | `sweet_frpc_*`, `sweet_cam` |

## Blockers found (none changed in the tree)
1. **The modem pool is inside the 26 kernel's RAM** -- DECIDED and done in the loader (not the ADT). `MODEM_POOL_BASE 0xE0000000` (16 MB) is unmanaged RAM in iOS 16's ADT, but in iOS 26 the kernel owns the biggest free block, and TrustZone and the modem only accept buffers below 0xF0000000, so the GPU's trick (cut the top of the ADT) does not apply. `base/xnuloader/loader.c` already hands the kernel a 128 MB block below 4 GB as `lowdma=`; it now also allocates 16 MB below `PLAT->modem_pool_max` (0xEFFFFFFF on sapphire, `platform_sapphire` in `loader.c` and `base/common/devices/sapphire.platform.c`) with `AllocateMaxAddress`, after the kernel's window is claimed, and passes it as boot-arg `modempool=0x<address>`. The kernel-side modem and audio code must take its pool from that boot-arg (no constant), and refuse to start without it. Not run on the phone: the first check is that the loader's log shows `modem pool <address>` and the kernel command line carries `modempool=`.
   * The enxerto path (`enxerto/loader.c`, fixed `phys_base`) has no such allocation and is not covered.
   * The GPU pool (0xfce00000, the ADT cut) assumes the ADT path; with xnuloader's own device tree `dram-size` is the whole free block, so `hakios.a610.cpp`'s "refuse if the ADT covers the pool" check will refuse. Give the GPU the same treatment (a loader allocation + `gpupool=`) before using `gpu=1` under UEFI.
2. **About 15 symbols the file calls are defined in iOS 16 files that are not in this tree:** `sweet_i2c1_*`, `sweet_spmi_*`, `sweet_qmi_*`, `sweet_pdp_*`, `sweet_ipa_*`, `sweet_audio_*`, `sweet_gpr_*`, `sweet_mic_want`, `sweet_frpc_*`, `sweet_cam`, `topaz_wlan_bss`. Their other end is the iOS 16 userland (`/dev/sweet-*` devices, the audio and Wi-Fi daemons), which speak a different protocol from this tree's `/dev/hakios-audio` (`redwood_audio.h`) and Apple80211 shim (`hakios.apple80211.c`, `hakios.sta.c`).
3. **Two Wi-Fi stacks.** This tree's `drivers/wifi/` is written for a WCN6750 over ath11k-style WLFW on SC7280; the iOS 16 one is a WCN3988 / ath10k-style radio (CE rings, HTC, WMI TLV, HTT 64-bit). They cannot share code below the Apple80211 layer; the radio has to come over whole, behind `hakios.apple80211.c`.

## Order that works
1. Pool: done (loader, `modempool=`); confirm on the phone.
2. SMEM + SMP2P + GLINK edge (lines 331-766, 908-1140) as `drivers/core/hakios.smem.c`; QRTR on top of this tree's `redwood_qrtr.h`.
3. ADSP boot + GPR + speaker (audio=2) against `/dev/hakios-audio`; codec supply = `hakios_rpm_ldo(14, 1800000, 650, 0/1)`.
4. WLFW + WLAN radio behind the Apple80211 layer; modem and IPA last.
The existing SC7280 drivers (`hakios.adsp.c`, `hakios.wifi.c`, `hakios.mpss.c`, ...) are guarded by `qcom,sm6225` and stay off on this phone.
