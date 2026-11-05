# HakiOS – Báo cáo công việc Codex

**Ngày lập:** 05/11/2026  
**Phạm vi:** các chỉnh sửa hiện có trong working tree từ phiên làm việc này và các thay đổi liên quan đến review RAM range, Quad-PTE, LSE, GICv3 và Enxerto loader.  
**Thiết bị:** Xiaomi Redmi Note 13 4G (`sapphire`), Qualcomm Snapdragon 685 / SM6225 Bengal, ARMv8.0-A, RAM dự kiến `0x80000000..0xc0000000` (1 GiB), granule MMU 4KB và Mach VM 16KB.

## Tóm tắt

Working tree hiện có **7 file được sửa**, chưa commit. Các thay đổi tập trung vào kiểm tra bounds của loader/inflate, thao tác RAM-range patcher an toàn hơn, sửa trường VA trong TLBI Quad-PTE, làm đúng địa chỉ cache clean cho LSE island, và cải thiện timeout/lỗi GICv3.

Đây là kiểm tra và chỉnh sửa tĩnh trong repo. Chưa có source tree XNU 12377 đã áp dụng các patch, chưa build kernel và chưa chạy lại trên thiết bị. Vì vậy chưa xác nhận được tích hợp với toàn bộ call graph XNU hay runtime.

## Những gì đã thay đổi

### 1. RAM range và patcher

**File:** `kernel/patch_ram_alignment.py`

- Giới hạn tìm assignment `first_avail = avail_start` vào phần thân `arm_vm_init()` thay vì tìm toàn file.
- Patcher fail closed nếu không tìm được duy nhất một định nghĩa hàm/assignment cần thiết hoặc nếu `avail_start` còn bị ghi sau điểm handoff.
- Thay cách regex thay toàn bộ đoạn từ `initialize_ram_ranges()` tới cuối `pmap_next_page()` bằng thao tác thay thân từng hàm đích: `initialize_ram_ranges`, `pmap_free_pages`, `pmap_free_pages_span`, `pmap_next_page_hi`, `pmap_next_page`.
- Việc thay từng thân hàm nhằm tránh xóa nhầm hàm, conditional compile hoặc nội dung khác nằm xen giữa trong `pmap.c`.

**Chưa xác minh:** parser nhận diện hàm chưa được chạy với source XNU thật. Logic xử lý range vẫn còn giả định về đơn vị/địa chỉ của `first_avail`, `avail_end`, `ml_static_vtop`, `PAGE_SIZE`, `atop()` và `ppnum_t`.

### 2. Quad-PTE TLBI

**File:** `kernel/0004-pmap-4kb-quad-pte.patch`

- Sửa mask VA operand trong `pmap_quad_tlb_invalidate()` từ 48 bit thành 44 bit: `0x00000FFFFFFFFFFF`, để không đưa các bit nằm ngoài trường VA của operand vào vùng TTL/RES0.
- Các barrier hiện có vẫn theo thứ tự `dsb ishst` → bốn `tlbi vale1is` → `dsb ish` → `isb`.

**Chưa sửa:** BBM cho thay đổi mapping valid, kiểm tra mọi writer PTE, alignment thực tế của `ptep`, ASID theo loại mapping và tương tác với `PMAP_UPDATE_TLBS()`. Không thêm BBM cục bộ vì helper hiện không đủ ngữ cảnh old PTE/lock/call-site để làm an toàn.

### 3. LSE branch islands

**File:** `kernel/0002-lse-branch-islands.patch`

- Tách địa chỉ dùng cho D-cache clean và I-cache invalidate.
- `dc cvau` hiện dùng physmap alias (`PHYSTOKV(pa)`) đã nhận lệnh ghi; `ic ivau` vẫn dùng VA thực thi.
- Cập nhật các call site của helper cache maintenance.

**Chưa sửa:** đồng bộ I-cache trên CPU khác, race khi nhiều CPU tìm/ghi cùng user code cave, pinning/COW/page-out của user mapping, rollback slot kernel bị reserve rồi thất bại, kích thước cache line hard-code và các giả định về layout `task`/`vm_map` trong patch cần xác nhận với XNU 12377.

### 4. Enxerto board, loader và inflate

**Files:** `enxerto/board.h`, `enxerto/loader.c`, `enxerto/inflate.c`

- Thêm `DRAM_SIZE = 0x40000000` cho Sapphire và dùng cửa sổ DRAM cố định để giới hạn RAM window/payload source.
- `loader_check()` kiểm tra range payload trước khi đọc header; thêm kiểm tra overflow và bounds cho RAM window, VA→PA segment, ADT/boot_args, overlap giữa segment và overlap với image Enxerto.
- Kiểm tra entry point căn 4 byte và nằm trong segment đầu (`__TEXT` theo giả định của `tools/mkpayload.py`). Payload hiện không mang quyền segment nên executable permission chưa được xác nhận trực tiếp từ metadata.
- `copy64()` sao chép phần đuôi 1–7 byte mà không đọc/ghi quá phần payload; `zero64()` xử lý cả byte đầu/cuối chưa căn 8 byte.
- Inflate destination capacity được đặt bằng `raw_size`, không phải toàn `vmsize`; vùng zero-fill bắt đầu ngay sau số byte inflate thực tế.
- Các phép kiểm tra trong `inflate.c` đổi sang dạng subtraction-safe để tránh overflow ở input/output count.

**Chưa sửa/xác minh:** TOCTOU nếu nguồn payload có thể bị DMA/thay đổi sau validation; ý nghĩa PA/VA khi so overlap; linker symbol `_head`/`_end` ở runtime; memory carve-outs khác ngoài DRAM; payload embedded dùng range cố định 128 MiB; executable protection của entry segment.

### 5. GICv3

**File:** `kernel/xnu-12377.121.6-dada.patch`

- Poll RWP và redistributor wake dùng timeout theo `CNTVCT_EL0` (19.2 MHz), thay vòng đếm cố định; timeout vẽ band đỏ.
- Đăng ký interrupt handler trên từng CPU sau redistributor wake và trước bật PPI/Group 1.
- Đọc lại `ICC_SRE_EL1`, cấu hình PMR/BPR và đặt EOImode phù hợp với handler đang dùng EOIR.
- `gic_v3_spi_mask()` trả `bool`, kiểm tra bit enable đã clear và kiểm tra RWP.
- SPI attach xử lý lỗi mask/settle, dọn pending/active/routing và giải phóng slot.
- Caller `lx_os_irq_attach()` giờ xử lý mask failure bằng cách clear trạng thái attached, detach SPI và trả `EIO`.

**Chưa sửa/xác minh:** một số caller khác vẫn bỏ qua kết quả `gic_v3_spi_mask()` do API của caller là `void`; detach còn vòng chờ `busy` không timeout; trình tự unwind khi mask thực sự không có hiệu lực cần review; semantics của XNU 12377 và cấu hình GIC phần cứng cần đối chiếu source/platform.

## File đã sửa

| File | Nội dung chính |
|---|---|
| `kernel/patch_ram_alignment.py` | Scope handoff vào `arm_vm_init`; thay riêng thân hàm pmap; fail closed khi mơ hồ |
| `kernel/0004-pmap-4kb-quad-pte.patch` | Mask TLBI VA 44 bit |
| `kernel/0002-lse-branch-islands.patch` | D-cache clean qua physmap alias, I-cache invalidate qua execute VA |
| `kernel/xnu-12377.121.6-dada.patch` | GIC timeout, handler per CPU, mask failure handling/cleanup |
| `enxerto/loader.c` | Bounds/overflow/overlap, copy/zero tail, inflate cap theo `raw_size` |
| `enxerto/inflate.c` | Bounds subtraction-safe cho inflate input/output |
| `enxerto/board.h` | Khai báo dung lượng DRAM Sapphire |

Không có file nào bị xóa trong diff hiện tại. Các patch nằm trong repo dưới dạng unified patch; source XNU đích không có trong workspace để xác nhận từng hunk trên tree đã patch.

## Kiểm tra đã thực hiện

- `git diff --check`: thành công, không báo lỗi whitespace. Git có cảnh báo chuẩn hóa LF/CRLF cho một số file.
- `python -m py_compile kernel/patch_ram_alignment.py`: thành công.
- `git apply --numstat` cho `0002-lse-branch-islands.patch`, `0004-pmap-4kb-quad-pte.patch`, `xnu-12377.121.6-dada.patch`: parser đọc được unified diff.
- Không chạy test suite, không build kernel, không chạy CI, không nạp image lên thiết bị.

## Rủi ro còn lại – ưu tiên

### Critical / High

1. **Điểm treo sau `pf:t2` chưa được xác nhận đã qua.** Cần source đang build của `arm_vm_init()`, `initialize_ram_ranges()`, caller `pmap_free_pages_span()`, cùng giá trị runtime của `first_avail`, `avail_start`, `avail_end`, `gPhysBase`.
2. **Quad-PTE BBM và writer coverage chưa được audit.** Cần `pmap_enter_options_internal`, `pmap_remove_options_internal`, `pmap_protect_options_internal`, `pmap_page_protect`, fast-fault, `pmap_enter_pte`, `write_pte_fast` và các macro TLBI/lock liên quan.
3. **LSE user cave vẫn có race.** Cần lock/lifecycle đúng của pmap và API đồng bộ instruction cache trên các CPU; không nên dựa vào atomic kernel slot counter để bảo vệ cave dùng chung.
4. **GIC mask failure vẫn chưa truyền được qua mọi API.** Xem caller trong ngữ cảnh interrupt/disable/teardown và vòng chờ busy khi detach.

### Medium

5. **RAM range heuristic còn dựa vào ngưỡng VA cứng và `ml_static_vtop()`.** Cần kiểm tra implementation và xác nhận input hợp lệ trước khi đổi.
6. **Loader chưa snapshot payload metadata.** Cần xác định ABL/fastboot/DMA có thể sửa nguồn payload trong lúc kiểm tra và copy hay không.
7. **Entry executable permission chưa được biểu diễn trong payload.** Cần xem `tools/mkpayload.py` và định dạng segment để kiểm tra quyền thực thi thay vì chỉ dựa vào segment đầu.
8. **Các giới hạn cache line/PA–VA trong LSE và identity assumptions trong loader cần xác nhận theo mapping thật.**

## Hướng phát triển đề xuất để nhờ anti review

1. **Ưu tiên gỡ hang boot:** cung cấp source XNU đã áp dụng patch cho RAM initialization và caller; ghi marker/giá trị từng biến trước và sau normalize, tránh panic/assert trong early boot.
2. **Audit Quad-PTE theo call graph:** lập danh sách mọi writer; xác định lock thật của XNU 12377; thiết kế BBM ở tầng có old/new descriptor, VA, pmap và lock context; xác định TLBI leaf/table và shootdown remote CPU.
3. **Audit LSE:** thay layout giả bằng API/type XNU thật; reserve slot sau preflight hoặc rollback có chứng minh; khóa/pin user mapping; ghi island → cache synchronization → patch site; xác định cơ chế cross-CPU I-cache synchronization.
4. **Audit GIC:** đối chiếu `ml_install_interrupt_handler()` với source 12377, kiểm tra mọi caller xử lý kết quả mask, hoàn thiện unwind và teardown có bounded wait/failure signal.
5. **Audit loader:** xác nhận memory map/carve-outs đáng tin của Sapphire, tính hợp lệ của linker symbols và payload source; xác định TOCTOU; mở rộng payload format/tool để biểu diễn segment permissions nếu cần.
6. **Sau khi có source đủ:** áp dụng patch trên tree sạch, build riêng từng nhóm thay đổi, rồi mới kiểm tra boot theo marker để khoanh vùng lỗi. CI và nạp máy do chủ dự án quản lý.

## Ghi chú về agent

Đã chia review thành bốn luồng đồng thời trong giới hạn bốn slot gồm agent chính: RAM patcher, Quad-PTE, LSE và loader; agent chính xử lý phần GIC. Quad-PTE/RAM review xác nhận cần source XNU cho các sửa sâu hơn; các sửa trực tiếp được giữ ở mức có thể căn cứ từ code patch hiện có.
