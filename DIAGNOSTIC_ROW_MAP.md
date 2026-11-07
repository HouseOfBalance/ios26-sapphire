# HakiOS Diagnostic Row Map & Physical Calibration Guide

**Target Hardware:** Xiaomi Redmi Note 13 4G (`sapphire`, SM6225 Bengal, Kryo 260 / Cortex-A73 Gold + A53 Silver)  
**Display:** 1080 × 2400 AMOLED Display  
**Framebuffer Geometry:** Physical base `0x5c000000`, Rowbytes `4320` bytes (32-bit ARGB, 1080 pixels/row)  
**Reference Anchor:** Enxerto Project Logo centered at `BLOCK_TOP = 600` (Lime green lemon on dark green background). Punch-hole camera centered at top bezel `y ≈ 80..130`.

---

## 1. Fast Diagnostic Reading Order (Post-Boot Inspection)

After booting with `fastboot boot enxerto-boot.img`:
1. **Check Exception Vectors (Rows 198 .. 216):**
   - Did an exception occur? Look at **Rows 198, 204, 210, 216, 220**.
   - If **none** of 198..220 are drawn, no sync exception or double fault fired in early boot.
2. **Check `pmap_next_page` Execution (Rows 306 .. 312):**
   - **Row 304 (Vivid Magenta):** POST-LOCK reached.
   - **Row 306 (White):** `pmap_next_page()` entered.
   - **Row 308 (Cyan):** `pmap_next_page()` returning `TRUE`.
   - **Row 309 (Dark Orange):** `pmap_next_page()` returning `FALSE`.
   - **Row 311 (Purple):** Call-site in `pmap_startup` successfully received return value.
   - **Row 312 (Deep Sky Blue):** First iteration (`i == 0`) of page init loop entered.
3. **Check Diagnostic Gauge (Rows 410 .. 440):**
   - **Row 410 (White, 4 rows):** Calibration marker.
   - **Rows 416..423 (8 rows):** 8 nibbles of `first_avail` (MSB first, using 16-color palette below).
   - **Rows 426..433 (8 rows):** 8 nibbles of `avail_end` (MSB first, using 16-color palette below).
   - **Row 436 (2 rows):** `need_ram_ranges_init` (Lime Green = 1, Vivid Red = 0).
   - **Row 440 (2 rows):** `VBAR_EL1` (Lime Green = Virtual Address $\ge \text{0xffff000000000000}$, Vivid Red = Physical Address).

---

## 2. 16-Color Nibble Palette Reference

Each row in the gauge represents one hexadecimal nibble (4 bits, values 0x0 through 0xF):

| Nibble | Color Name | Hex Code (ARGB) | Visual Description |
|:------:|:-----------|:---------------:|:-------------------|
| `0x0`  | Black      | `0x00000000`    | Dark / Off |
| `0x1`  | Navy       | `0x00000080`    | Dark Blue |
| `0x2`  | Green      | `0x00008000`    | Standard Green |
| `0x3`  | Teal       | `0x00008080`    | Blue-Green |
| `0x4`  | Maroon     | `0x00800000`    | Dark Red |
| `0x5`  | Purple     | `0x00800080`    | Deep Purple |
| `0x6`  | Olive      | `0x00808000`    | Brown-Green |
| `0x7`  | Silver     | `0x00C0C0C0`    | Light Gray |
| `0x8`  | Gray       | `0x00808080`    | Medium Gray |
| `0x9`  | Blue       | `0x000000FF`    | Bright Blue |
| `0xA`  | Lime       | `0x0000FF00`    | Bright Lime Green |
| `0xB`  | Cyan       | `0x0000FFFF`    | Bright Aqua / Cyan |
| `0xC`  | Red        | `0x00FF0000`    | Bright Red |
| `0xD`  | Magenta    | `0x00FF00FF`    | Bright Pink / Magenta |
| `0xE`  | Yellow     | `0x00FFFF00`    | Bright Vivid Yellow |
| `0xF`  | White      | `0x00FFFFFF`    | Pure White |

---

## 3. Comprehensive Diagnostic Ladder Map

All rows are strictly non-overlapping and verified:

| Row Range | Height | Color Name | Hex (ARGB) | Milestone Description |
|:---------:|:------:|:-----------|:----------:|:----------------------|
| `0..9`    | 10     | Yellow     | `0x00FFFF00` | CP1: `start_first_cpu` entry in `start.s` |
| `50..59`  | 10     | Orange     | `0x00FFA500` | CP2: Entered `arm_init` in C |
| `60..69`  | 10     | Green      | `0x0000FF00` | CP3: Past CP30, about to slide/rebase |
| `70..79`  | 10     | Purple     | `0x00800080` | CP4: Past `arm_slide_rebase_and_sign_image` |
| `80..89`  | 10     | Yellow     | `0x00FFFF00` | CP5: Past `arm_static_if_init` |
| `184`     | 2      | Cyan       | `0x0000FFFF` | `kernel_bootstrap` entry |
| `186`     | 2      | Hot Pink   | `0x00FF69B4` | `kernel_bootstrap_thread` entry |
| `194`     | 2      | Cyan       | `0x0000FFFF` | `STARTUP_SUB_LOCKS` (Subsystem 3 passed) |
| `198..201`| 4      | Cyan+EC    | `0x00ECFFFF` | `el1_sp0_synchronous_vector_long` entry (non-trap sync exception) |
| `204..207`| 4      | Vivid Yellow | `0x00FFFF00` | `Lcorrupt_stack` in `CHECK_KERNEL_STACK` (TPIDR_EL1 == 0 or corrupt SP) |
| `210..213`| 4      | Vivid Red  | `0x00FF0000` | `el1_sp1_synchronous_vector_long_spill_failed` (Double Fault spin `wfe`) |
| `216..219`| 4      | Dark Orange | `0x00FF8C00` | `sleh_invalid_stack` entered |
| `220..223`| 4      | Magenta    | `0x00FF00FF` | On-screen early panic text rendered via `hk_render_screen_asm` |
| `250..251`| 2      | White      | `0x00FFFFFF` | `initialize_ram_ranges` entered |
| `256..257`| 2      | Lime       | `0x0000FF00` | `initialize_ram_ranges` completed |
| `258..259`| 2      | Cyan       | `0x0000FFFF` | `pmap_free_pages_span` entered |
| `260..261`| 2      | Gold       | `0x00FFD700` | `pmap_free_pages_span` computed |
| `280..285`| 6      | Lime       | `0x0000FF00` | `vm_pages` stolen successfully (`pmap_steal_freeable_memory`) |
| `288..293`| 6      | Dark Orange | `0x00FF8C00` | `vm_pages_end` computed (`vm_page_get(npages)`) |
| `296..301`| 6      | Vivid Yellow | `0x00FFFF00` | `PRE-LOCK`: Before acquiring `vm_free_page_lock()` |
| `304..305`| 2      | Vivid Magenta | `0x00FF00FF` | `POST-LOCK`: `vm_free_page_lock()` acquired |
| `306..307`| 2      | White      | `0x00FFFFFF` | `pmap_next_page(&phys_page)` entered |
| `308`     | 1      | Cyan       | `0x0000FFFF` | `pmap_next_page` returning `TRUE` |
| `309`     | 1      | Dark Orange | `0x00FF8C00` | `pmap_next_page` returning `FALSE` |
| `310`     | 1      | *(Gap)*    | — | Reserved separator gap |
| `311`     | 1      | Purple     | `0x00800080` | `pmap_startup` call-site returned from `pmap_next_page` (`i == 0`) |
| `312..315`| 4      | Deep Sky Blue | `0x0000BFFF` | `pmap_startup` loop body entered (`i == 0`) |
| `316..319`| 4      | Lime       | `0x0000FF00` | `vm_pages_first_pnum` set |
| `320..323`| 4      | Magenta    | `0x00FF00FF` | `patch_low_glo_vm_page_info` completed |
| `324..327`| 4      | Yellow     | `0x00FFFF00` | Before `vm_page_init(0)` |
| `328..331`| 4      | Orange     | `0x00FF8C00` | After `vm_page_init(0)` |
| `348..351`| 4      | Lime       | `0x0000FF00` | Loop finished, `vm_free_page_unlock()` released |
| `360..361`| 2      | Lime Green | `0x0000FF00` | `vm_page_bootstrap` returned |
| `410..413`| 4      | White      | `0x00FFFFFF` | Gauge Calibration Reference Marker |
| `416..423`| 8      | Palette    | Variable   | 8 nibbles of `first_avail` (MSB = 416, LSB = 423) |
| `426..433`| 8      | Palette    | Variable   | 8 nibbles of `avail_end` (MSB = 426, LSB = 433) |
| `436..437`| 2      | Lime / Red | `0x0000FF00`/`0x00FF0000` | `need_ram_ranges_init` flag (Lime = 1, Red = 0) |
| `440..441`| 2      | Lime / Red | `0x0000FF00`/`0x00FF0000` | `VBAR_EL1` alignment (Lime = VA, Red = PA) |
| `500..503`| 4      | Goldenrod  | `0x00DAA520` | `bsd_init` entered |
| `615..618`| 4      | Spring Green | `0x0000FF7F` | `vfs_mountroot` succeeded (Mockfs RAMDisk mounted) |
| `620..623`| 4      | Deep Sky Blue | `0x0000BFFF` | `bsd_utaskbootstrap` created PID 1 task |
| `630..633`| 4      | Dark Orange | `0x00FF8C00` | `bsdinit_task` thread running (clears TPIDR_EL0 bit 63) |
| `640..643`| 4      | Magenta    | `0x00FF00FF` | `/sbin/launchd` loaded, handoff to EL0 |
| `650..653`| 4      | Vivid Yellow | `0x00FFFF00` | PID 1 first syscall (`svc #0x80`) captured in kernel |
