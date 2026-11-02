/*
 * pac_patch.c - Runtime Trap-and-Self-Patch Engine for ARMv8.0 (No FEAT_PAuth)
 * Part 1 Implementation for HakiOS / XNU-12377 on Snapdragon 685 (Cortex-A73/A53)
 */

#if defined(__clang__)
#pragma clang attribute push (__attribute__((target("branch-protection=none"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC target ("branch-protection=none")
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if defined(__has_include) && __has_include(<arm/thread.h>)
#include <arm/thread.h>
#include <mach/machine/thread_status.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <vm/vm_map.h>
#else
typedef struct arm_saved_state arm_saved_state_t;
typedef void *thread_t;
typedef struct task { char pad[32]; struct vm_map *map; } *task_t;
typedef struct vm_map { char pad[40]; void *pmap; } *vm_map_t;
extern thread_t current_thread(void);
extern task_t get_threadtask(thread_t th);
extern uint64_t get_saved_state_reg(arm_saved_state_t *state, unsigned int reg);
extern void set_saved_state_reg(arm_saved_state_t *state, unsigned int reg, uint64_t val);
extern uint64_t get_saved_state_sp(arm_saved_state_t *state);
extern void set_saved_state_sp(arm_saved_state_t *state, uint64_t sp);
extern uint64_t get_saved_state_pc(arm_saved_state_t *state);
extern void set_saved_state_pc(arm_saved_state_t *state, uint64_t pc);
typedef uint64_t user_addr_t;
typedef uint64_t vm_size_t;
typedef uint64_t vm_offset_t;
#endif

#include "pac_decode.h"

extern unsigned long gVirtBase, gPhysBase;
#define PHYSTOKV(pa) ((uint64_t)(pa) + (gVirtBase - gPhysBase))

extern uint64_t kvtophys(vm_offset_t va);
extern uint64_t pmap_find_phys(void *pmap, uint64_t va);
extern int copyin(const user_addr_t user_addr, void *kernel_addr, vm_size_t nbytes);
extern void loader_log_line(const char *line);

/* Thống kê hiệu năng runtime */
static uint64_t pac_patch_count = 0;
static uint64_t pac_emul_count = 0;
static uint64_t pac_smp_races = 0;

/*
 * Ghi đè mã máy tại Physical Address và đồng bộ Cache toàn bộ Core
 */
static void
patch_memory_phys(uint64_t pa, uint64_t va, uint32_t new_insn)
{
	volatile uint32_t *p = (volatile uint32_t *)PHYSTOKV(pa);
	*p = new_insn;

	/*
	 * Đồng bộ hóa cache bắt buộc trên ARMv8-A:
	 * 1. dc cvau: đẩy dữ liệu vừa ghi từ D-Cache ra Point of Unification.
	 * 2. dsb ish: đảm bảo lệnh ghi hoàn tất trên toàn bộ các core trong cụm SMP.
	 * 3. ic ivau: xóa bỏ dòng lệnh tương ứng trong I-Cache.
	 * 4. dsb ish: đảm bảo lệnh xóa I-Cache đã broadcast xong.
	 * 5. isb    : làm rỗng instruction pipeline của core hiện tại để nạp lại lệnh mới.
	 */
	__asm__ volatile (
		"dc cvau, %0 \n"
		"dsb ish     \n"
		"ic ivau, %0 \n"
		"dsb ish     \n"
		"isb         \n"
		:: "r"(va) : "memory"
	);
}

/*
 * Xử lý lệnh LDRAA / LDRAB không encode được thành 1 lệnh duy nhất (Phải Emulate)
 */
static bool
emulate_ldra(arm_saved_state_t *state, const pac_result_t *res, bool is_user)
{
	uint64_t base_addr = (res->rn == 31) ? 
		(uint64_t)get_saved_state_sp(state) : 
		(uint64_t)get_saved_state_reg(state, res->rn);

	uint64_t target_addr = base_addr + (int64_t)res->off;
	uint64_t loaded_val = 0;

	if (is_user) {
		if (copyin((user_addr_t)target_addr, &loaded_val, sizeof(loaded_val)) != 0) {
			/* Lỗi truy cập bộ nhớ userland: trả về false để kernel xử lý */
			return false;
		}
	} else {
		loaded_val = *(volatile uint64_t *)target_addr;
	}

	/* Ghi giá trị đọc được vào thanh ghi đích Xt */
	if (res->rt != 31) {
		set_saved_state_reg(state, res->rt, loaded_val);
	}

	/* Nếu là chế độ pre-index writeback (Xn!): cập nhật lại thanh ghi cơ sở Xn */
	if (res->writeback) {
		if (res->rn == 31) {
			set_saved_state_sp(state, target_addr);
		} else {
			set_saved_state_reg(state, res->rn, target_addr);
		}
	}

	/* Tiến con trỏ lệnh PC qua lệnh LDRAA (4 bytes) */
	uint64_t pc = (uint64_t)get_saved_state_pc(state);
	set_saved_state_pc(state, pc + 4);
	return true;
}

/*
 * Main Entry Point: Được gọi từ sleh_synchronous khi gặp ESR_EC_UNCATEGORIZED (0x00)
 * Trả về true: Đã xử lý / self-patched -> XNU chỉ cần return và eret để CPU chạy lại
 * Trả về false: Không phải lệnh PAC -> XNU tiếp tục đẩy sang SIGILL (EXC_BAD_INSTRUCTION)
 */
bool
pac_patch_handle_trap(arm_saved_state_t *state, bool is_user)
{
	uint64_t pc = (uint64_t)get_saved_state_pc(state);
	uint32_t insn = 0;
	uint64_t pa = 0;
	void *pmap = NULL;

	/* 1. Đọc mã lệnh insn 32-bit từ vị trí PC */
	if (is_user) {
		if (copyin((user_addr_t)pc, &insn, sizeof(insn)) != 0) {
			return false;
		}
		task_t task = get_threadtask(current_thread());
		if (task && task->map) {
			pmap = (void *)task->map->pmap;
		}
		if (!pmap) return false;
		pa = pmap_find_phys(pmap, pc);
	} else {
		insn = *(volatile uint32_t *)pc;
		pa = kvtophys(pc);
	}

	if (!pa) return false;

	/*
	 * 2. SMP RACE DEFENSE:
	 * Nếu core khác vừa vá xong lệnh này tại RAM, opcode đọc được tại thời điểm này
	 * đã không còn là lệnh PAC nữa. Ta coi như đã xử lý và quay về để CPU nạp lại!
	 */
	if (insn == A64_NOP || insn == A64_RET || insn == A64_ERET || 
	    (insn & 0xfffffc1fu) == A64_BR_BASE || (insn & 0xfffffc1fu) == A64_BLR_BASE ||
	    (insn & 0xffffffe0u) == A64_MOV_X_XZR ||
	    (insn & 0xffc00000u) == A64_LDR_UIMM ||
	    (insn & 0xffe00000u) == 0xf8400000u) {
		pac_smp_races++;
		return true;
	}

	/* 3. Giải mã qua bộ PAC Decoder thuần ARM ARM */
	pac_result_t res;
	pac_action_t act = pac_decode(insn, &res);

	switch (act) {
	case PAC_PATCH:
		/* Ghi đè mã máy tại RAM qua Physical Mapping */
		patch_memory_phys(pa, pc, res.new_insn);
		pac_patch_count++;
		/*
		 * KHÔNG tăng PC: CPU khi eret sẽ thực thi lại chính địa chỉ PC này,
		 * nhưng lúc này lệnh đã trở thành NOP, RET, BLR, LDR... gốc ARMv8.0!
		 */
		return true;

	case PAC_EMULATE_LDRA:
		if (!emulate_ldra(state, &res, is_user)) {
			return false;
		}
		pac_emul_count++;
		return true;

	case PAC_NOT_PAC:
	default:
		return false;
	}
}

#if defined(__clang__)
#pragma clang attribute pop
#endif
