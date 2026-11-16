# iOS 26 on Xiaomi Redmi Note 13 4G (Sapphire / SM6225)

English | [简体中文](README.zh-CN.md)

This project boots **Apple's open-source XNU kernel (xnu-12377.121.6, iOS 26)** on the **Xiaomi Redmi Note 13 4G** (codename **sapphire**, **Snapdragon 685 / SM6225-AD Bengal**, ARMv8.0-A baseline: 4× Cortex-A73 + 4× Cortex-A53) and runs the **iOS 26 arm64e user space** on top of it.

Built on **HakiOS**, with **arm64e**, **iOS 26**, and **SM6225 (Sapphire)** support added.

> ⚠️ **Experimental.** Requires an unlocked bootloader. Can cause data loss or brick the device.
> Only proceed if you are comfortable with low-level tinkering and accept all risks.

---

## Hardware Target

| Spec | Detail |
|------|--------|
| **Device** | Xiaomi Redmi Note 13 4G (sapphire) |
| **SoC** | Snapdragon 685 (SM6225-AD Bengal) |
| **CPU** | 8 cores: 4× Cortex-A73 (Gold, 2.8 GHz) + 4× Cortex-A53 (Silver, 1.9 GHz) |
| **Architecture** | ARMv8.0-A (no PAC/LSE in hardware) |
| **GPU** | Adreno 610 (a610) |
| **RAM** | 6/8 GB LPDDR4X |
| **Storage** | UFS 2.2 |
| **Display** | 1080×2400 AMOLED |
| **Modem** | X11 LTE (integrated) |
| **Wi-Fi/BT** | WCN6750 (Wi-Fi 6E + BT 5.2) |
| **Audio** | WCD938x codec + AW88261 speaker amp |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Android Bootloader (ABL)                     │
└─────────────────────────────┬───────────────────────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              Mu-Silicium UEFI / Fastboot (DWC3)                 │
│  ─────────────────────────────────────────────────────────────  │
│  • GOP framebuffer (1080×2400 @ 0x5c000000)                      │
│  • GICv3 init (GICD 0x0f200000, GICR 0x0f300000)                 │
│  • RPMh / SPMI / PMIC (PM6125 PON, PM7250B charger)              │
│  • UFS / UFSHCI for partition access                            │
│  • Apps SMMU stream mapping (0x740) for fastboot gadget         │
└─────────────────────────────┬───────────────────────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    Enxerto Bootloader (EL1)                     │
│  ─────────────────────────────────────────────────────────────  │
│  • Stage-1 4KB granule MMU (L1/L2/L3 tables, MAIR/TCR/SCTLR)    │
│  • UFS polling driver (dc_clean/flush/inval, 64-byte UTRD)      │
│  • DWC3 gadget (cache_ci/inv, Device-nGnRnE)                    │
│  • Payload parser (SWEETXNU magic, chained fixups, DEFLATE)     │
│  • ADT/boot_args placement, modempool/gpupool allocation        │
│  • mmu_off_jump → XNU entry (cache clean + DSB + ISB)           │
└─────────────────────────────┬───────────────────────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    XNU Kernel (arm64e, iOS 26)                  │
│  ─────────────────────────────────────────────────────────────  │
│  ┌─ PAC Trap-and-Self-Patch (ARMv8.0 emulation)                │
│  │   retaa/retab→ret, braa/blraa→br/blr, pac*→nop, ldraa→ldr   │
│  ├─ LSE Branch Islands + LL/SC (ARMv8.0 atomics)                │
│  │   CAS/CASP/SWP/LDADD/CLR/EOR/SET/SMAX/SMIN/UMAX/UMIN        │
│  ├─ 4KB Quad-PTE Pmap (16KB VM → 4KB HW granule)               │
│  ├─ DSC v5 slide info v5 authenticated pointer stripping        │
│  └─ Cortex-A73 PAC sysreg stub (APDAKey/APDBKey/APGAKey)        │
│  ─────────────────────────────────────────────────────────────  │
│  Drivers: Adreno 610 | UFS | ADSP | MPSS Modem | IPA | WCN6750  │
│  Audio: Speaker (AW88261) | Mic (SoundWire/WCD938x)             │
│  Power: RPMh | CPufreq | Thermal | Battery (PM7250B)            │
└─────────────────────────────┬───────────────────────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                  iOS 26 arm64e User Space                       │
│  ─────────────────────────────────────────────────────────────  │
│  • launchd, SpringBoard, backboardd, dyld                       │
│  • Metal → metal2vulkan → Mesa Turnip → NativeAdreno KMD        │
│  • Audio HAL plug-in (hakios.audio)                             │
│  • Camera, Haptics, Torch, Bluetooth, Wi-Fi                     │
└─────────────────────────────────────────────────────────────────┘
```

---

## Key Technical Solutions

### ARMv8.3 PAC Emulation (Trap-and-Self-Patch)
Hardware lacks PAC instructions. Kernel traps `UNDEF` on PAC ops, decodes instruction, patches in-place to NOP/RET/BLR equivalents.
- **Files**: `base/xnu/files/osfmk/arm64/pac_patch.c`, `pac_decode.h`
- **Handles**: `retaa/retab`, `braa/blraa`, `pacia/pacib/pacda/pacdb`, `pacga`, `ldraa/ldrab`, `autia/autib/autda/autdb`

### ARMv8.1 LSE Atomics via Branch Islands + LL/SC
Hardware lacks LSE. Traps LSE instructions, generates branch island trampolines using `ldaxr/stlxr` LL/SC loops.
- **Files**: `base/xnu/files/osfmk/arm64/lse_patch.c`, `lse_decode.h`, `lse_hooks.h`
- **Operations**: CAS, CASP, SWP, LDADD/CLR/EOR/SET/SMAX/SMIN/UMAX/UMIN (8/16/32/64/128-bit)

### 4KB Page Granule Pmap Rewrite
XNU defaults to 16KB pages; SM6225 only supports 4KB/64KB. Implements quad-PTE emulation (4× 4KB PTEs per 16KB VM page).
- **Files**: `base/xnu/files/osfmk/arm64/pmap.c` (patched via `0004-pmap-4kb-quad-pte.patch`)
- **Enxerto MMU**: `enxerto/mmu.c` — Stage-1 4KB L1/L2/L3 tables, WB/Device mappings, MAIR/TCR/SCTLR config

### Bootloader Chain
- **Enxerto** (`enxerto/`): Pure Python build, AArch64 GCC cross-compile, boot.img v4 (GKI layout)
- **Payload** (`tools/mkpayload.py`): Packs XNU Mach-O + ADT + ramdisk, resolves chained fixups, DEFLATE compression
- **Loader Return** (`base/xnu/files/osfmk/arm64/loader_return.c`): UEFI return channel, ring log, hv_wdt_pet, memory reporting

### Drivers (hakios.*)
| Driver | File | Function |
|--------|------|----------|
| **Adreno GPU** | `drivers/gpu/hakios.adreno.c` | 3858 lines, 22-step bring-up, SMMU Stage-1, RPMh rails |
| **UFS** | `drivers/storage/hakios.ufshc.c` | Read-only, whitelist cmd filter, 32 slots, poller thread |
| **ADSP Audio** | `drivers/audio/hakios.adsp.c` | 3342 lines, 11-step: SMEM, firmware, GLINK, QRTR, APR |
| **Speaker** | `drivers/audio/hakios.speaker.c` | AW88261 amp, I2C bit-bang, ACF profile, guard (Q30) |
| **Microphone** | `drivers/audio/hakios.mic.c` | SoundWire, WCD938x, ADC1, ASM/ADM/AFE streams |
| **Modem (MPSS)** | `drivers/modem/hakios.mpss.c` | 6431 lines, 11-step: SMEM, firmware, TrustZone, GLINK, QMI |
| **IPA Data Path** | `drivers/modem/hakios.ipa.c` | GSI rings, endpoints, QMAP framing |
| **Wi-Fi (WCN6750)** | `drivers/wifi/hakios.wifi.c` | 4480 lines, 16-step, ath11k-style WLFW QMI |
| **Camera** | `drivers/camera/hakios.camera.c` | 4310 lines, 4 sensors, CCI I2C, CSIPHY/CSID/IFE, SMMU |
| **Battery/PMIC** | `drivers/power/hakios.battery.c`, `hakios.pmic.c` | SPMI PM7250B, 444 peripherals, RTC |
| **Thermal** | `drivers/power/hakios.thermal.c` | 27 sensors, 2 TSENS blocks, 105°C trip |
| **CPufreq/RPM** | `drivers/power/hakios.cpufreq.c`, `hakios.rpm.c` | EPSS, Gold cluster max, DDR vote, GLINK rpm_requests |

---

## Repository Structure

```
├── .github/workflows/          # CI/CD: build-boot, build-xnu, build-all, build-rootfs
├── kernel/                     # XNU patches + apply.sh
│   ├── xnu-12377.121.6-dada.patch      # Base patch (Dada → Sapphire)
│   ├── 0001-pac-trap-and-self-patch.patch
│   ├── 0002-lse-branch-islands.patch
│   ├── 0003-sleh-hook.patch
│   ├── 0004-pmap-4kb-quad-pte.patch
│   └── patch_*.py               # VM/CPU/BSD/IOKit fixes (vm7, ram, breadcrumbs, timer, etc.)
├── base/                       # HakiOS kernel repo with Sapphire changes
│   ├── common/devices/sapphire.platform.c   # Platform def (io_base, GIC, cores, modem_pool_max)
│   ├── xnu/files/osfmk/arm64/               # PAC/LSE/pmap/loader_return
│   ├── xnu-deps/                            # Build deps, SDK overlay, toolchain
│   └── xnuloader/                           # UEFI loader (base/xnuloader/loader.c)
├── enxerto/                    # Bootloader (AArch64, boot.img v4)
│   ├── build.py                 # Pure Python build & packaging
│   ├── start.S                  # ARM64 Linux Image header, vector table, mmu_off_jump
│   ├── mmu.c/h                  # Stage-1 4KB MMU
│   ├── loader.c                 # Payload validation, segment copy, MMU enable
│   ├── ufs.c                    # UFSHCI polling driver
│   ├── usb_dwc3.c               # DWC3 gadget (fastboot)
│   └── pmic.c                   # SPMI PM6125/PM7250B
├── drivers/                    # All hakios.* drivers (see table above)
├── tools/                      # Build & utility scripts
│   ├── mkpayload.py             # Kernel + ADT + ramdisk → payload.bin
│   ├── mkramdisk.py             # Mockfs ramdisk for md0
│   ├── dsc_flatten_v2.py        # Dyld shared cache flattener
│   ├── renegade/                # RootFS builder from IPSW
│   └── verify_*.py              # Kernel markers, ADT validation
├── sapphire.adt                # Device tree for sapphire
└── PORTING_*.md                # Architecture docs (see below)
```

---

## Documentation

| File | Description |
|------|-------------|
| `PORTING_SOLUTIONS_ARM64E_SM6225.md` | Master architecture doc: PAC/LSE/pmap/bootloader design |
| `PORTING_PLAN_AUDIO_WIFI_SM6225.md` | Audio/Wi-Fi/Modem porting plan + 3 blockers |
| `NOTES_SAPPHIRE_HARDWARE.md` | iOS 16 port analysis: modem pool, ADSP, WCN3988, 15 missing symbols |

---

## Building (GitHub Actions)

### Quick Start
1. Go to **Actions** tab on GitHub
2. Run **`build-all.yml`** workflow (dispatch)
3. Download `hakios-26-sapphire-flash-package.zip` artifact
4. Unzip and run `flash-all.sh` (Linux/macOS) or `flash-all.bat` (Windows)

### Workflows
| Workflow | Purpose |
|----------|---------|
| `build-boot.yml` | Full boot chain: XNU kernel + enxerto + payload → boot.img |
| `build-xnu-12377.yml` | Kernel-only build (2 jobs: build-kernel → build-boot) |
| `build-all.yml` | Master: boot + rootfs (userdata.simg from IPSW) + flash package |
| `build-rootfs.yml` | RootFS only: fetch IPSW → Renegade + DSC flatten → userdata.simg |

### Artifacts Produced
- `enxerto-boot.img` — Main boot image (boot.img v4 / GKI)
- `charge-test.img` — Charging mode
- `bergamota-fastboot.img` / `bergamota-fastboot-flash.img` — Fastboot mode
- `payload.bin` — XNU kernel + ADT + ramdisk
- `mach.development.sapphire` — Raw kernel Mach-O
- `userdata.simg` — 16GB sparse rootfs (from IPSW)
- `flash-all.sh` / `flash-all.bat` — Complete flash scripts

---

## Boot Arguments (kernel cmdline)

```
-v debug=0xc serial=0 cpus=1 keepsyms=1 -enable_kprintf_spam
sweetearlyfb=1 sweethidtest=3 ipc_legacy=1
cs_enforcement_disable=1 amfi_get_out_of_my_way=1 rootless=0
rd=md0 rpm=1 hv_wdt_pet=1 modempool= gpupool=
```

| Arg | Purpose |
|-----|---------|
| `modempool=` | 16 MB below 0xF0000000 for TrustZone/modem (allocated by loader) |
| `gpupool=` | GPU pool allocation (allocated by loader) |
| `rpm=1` | Enable RPMh power management |
| `hv_wdt_pet=1` | Enable hypervisor watchdog petting |
| `rd=md0` | Boot from Mockfs ramdisk (md0) |

---

## Flashing

```bash
# Device must be in fastboot mode with unlocked bootloader
fastboot flashing unlock
# Confirm on device screen

# Flash all partitions
./flash-all.sh      # Linux/macOS
flash-all.bat       # Windows
```

**flash-all.sh does:**
1. Flash `enxerto-boot.img` → `boot_a` + `boot_b`
2. Flash `charge-test.img` → `recovery`
3. Flash `userdata.simg` (sparse, 512MB chunks) → `userdata`
4. Optionally flash `bergamota-fastboot.img` → `dtbo`
5. Reboot

---

## Current Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| **CPU/SMP** | ✅ Works | 8 cores up, Cortex-A73/A53, GICv3 |
| **Boot (Enxerto → XNU)** | ✅ Works | 4KB MMU, payload, ADT, boot_args |
| **Memory/Pmap** | ✅ Works | 4KB quad-PTE, WB/Device mappings |
| **PAC/LSE Emulation** | ✅ Works | Trap-and-self-patch, branch islands |
| **UFS Storage** | ✅ Read-only | Whitelist cmd filter, polling |
| **Display/FB** | ✅ Works | 1080×2400 AMOLED, GOP handoff |
| **ADSP Audio** | 🟡 In progress | SMEM/GLINK/QRTR/APR bring-up |
| **Speaker** | 🟡 In progress | AW88261 I2C, guard Q30 |
| **Microphone** | 🟡 In progress | SoundWire WCD938x |
| **Modem (MPSS)** | 🟡 In progress | Firmware load, TrustZone, GLINK, QMI |
| **IPA Data** | 🟡 In progress | GSI rings, QMAP |
| **Wi-Fi (WCN6750)** | 🟡 In progress | ath11k WLFW, firmware checksum |
| **Camera** | 🟡 In progress | 4 sensors, CCI I2C, SMMU |
| **GPU (Adreno 610)** | 🟡 In progress | 22-step bring-up, SMMU, RPMh rails |
| **Battery/PMIC** | ✅ Works | SPMI PM7250B, UEFI cache fallback |
| **Thermal** | ✅ Works | 27 sensors, 105°C trip |
| **CPufreq/RPMh** | 🟡 In progress | EPSS, Gold max, DDR vote |

---

## Known Blockers

1. **Modem pool in kernel RAM** — 16 MB below 0xF0000000 must be allocated by loader (`modempool=` boot-arg). Enxerto path not yet covered.
2. **15 missing `sweet_*` symbols** — iOS 16 userland daemons (`/dev/sweet-*`) not present. Stubs in `drivers/core/hakios.stubs.c` panic on call.
3. **Dual Wi-Fi stacks** — WCN3988 (ath10k) vs WCN6750 (ath11k). Cannot share code below Apple80211 layer.

See `PORTING_PLAN_AUDIO_WIFI_SM6225.md` for details.

---

## License

- **Kernel (XNU patches)**: APSL 2.0
- **HakiOS base (base/, userland/)**: APSL 2.0 (HakiOS)
- **Enxerto bootloader**: MIT
- **UEFI (uefi-dada/)**: BSD-2-Clause-Patent
- **GPU patches (gpu2/)**: Respect upstream licenses (see NOTICE.md)
- **Tools**: MIT

No Apple, Qualcomm, or Xiaomi binaries/firmware included.

---

## Acknowledgements

- **HakiOS** — Foundation of this project
- **UniXNU** — NativeAdreno/Metal, Ath12kWifi, Snapdragon platform reference
- **Data A Live** — GPU, audio, CPU clock reference for iOS on Snapdragon
- **metal2vulkan** — Metal → Vulkan translation layer
- **Mesa (Turnip/freedreno)** — Vulkan driver for Adreno
- **Project Silicium / Mu-Silicium** — UEFI firmware
- **uefi-dada** — Mu-Silicium port reference
- **Linux msm/ath12k/qcom drivers** — Register layouts, bring-up order

---

## Disclaimer

Apple, iOS, Metal are trademarks of Apple Inc.  
Qualcomm, Snapdragon, Adreno are trademarks of Qualcomm Inc.  
Xiaomi, Redmi are trademarks of Xiaomi Inc.  

This project is not affiliated with, endorsed by, or sponsored by any of the above companies.