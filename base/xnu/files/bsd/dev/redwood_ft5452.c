/*
 * The touch screen of the Redmi Note 13 4G (sapphire, SM6225 / "Bengal"): a FocalTech FT-series
 * controller (chip id 0x54:0x52, FT5452 / FT3519T firmware) at I2C address 0x38 on QUP0 serial
 * engine 2, read by polling the engine's FIFOs, as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with boot-arg touchscreen=2: every address below belongs to this phone
 * (redwood_goodix.c, touchscreen=1, is the POCO X5 Pro's SPI controller and must not run here).
 *
 * Ported from the iOS 16 port of the same phone (xnu/patches 0083, 0086, 0087, 0089, 0090, 0140,
 * 0285, 0300, 0312-0314: bsd/dev/sweet_touch.c), which has run on the hardware; the pins, clocks and
 * transfer code are its own, the report goes to redwood_touch_post() instead of the HID service.
 *
 *   serial engine   QUP0 SE2 at 0x04a88000, I2C firmware loaded by the ABL (protocol 3), 400 kHz
 *   clocks          GCC 0x01400000: QUP0 wrapper + SE2 vote (0x7900c, bits 0x13c0), SE2 RCG at 0x1f3a8
 *                   (CFG +4, CBCR -4), XO / 1 = 19.2 MHz
 *   pins            TLMM 0x00400000, WEST tile 0x00500000: GPIO 6/7 SDA/SCL (function 1), GPIO 80 IRQ
 *                   (low while the IC has a frame), GPIO 86 reset (low = reset); EAST tile 0x00d00000:
 *                   GPIO 36 AVDD enable (no regulator)
 *   report          register 0, 63 bytes: [2] & 15 = contact count, then six bytes a contact from
 *                   byte 3: event (bit 7:6), 12 bits of X, id (bit 7:4 of byte 2), 12 bits of Y, byte 4
 *                   the two fraction bits of each; a count above 10 with 0xff records = no contact
 *   raw range       0..2159 x 0..2399 with two fraction bits (boot-args touch_rx=8640 touch_ry=9600)
 *
 * Boot-args: touch_delay=S (seconds before the bus is touched, default 45 as on the iOS 16 port),
 * touch_irq=0 (poll on a grid instead of looking at GPIO80), touch_fast=0, touch_quiet_ms=N,
 * touch_stale_ms=N, touch_fbits=2|4, touch_rx=, touch_ry=, touch_rawlog=N.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

void redwood_ft5452_init(void);

#define TOUCH_SE_PA      0x04a88000u /* QUP0 SE2 */
#define TOUCH_GCC_PA     0x01400000u
#define TOUCH_WEST_PA    0x00500000u /* TLMM 0x400000 + WEST tile: GPIO 6/7/80/86 */
#define TOUCH_EAST_PA    0x00d00000u /* TLMM 0x400000 + EAST tile: GPIO 36 */
#define TOUCH_GCC_VOTE   0x7900cu    /* QUP0 wrapper + SE2 vote, bits 0x13c0 */
#define TOUCH_RCG        0x1f3a8u    /* QUP0 S2 CMD; CFG +4, CBCR -4 (gcc-khaje.c) */
#define TOUCH_PIN_RESET  86u
#define TOUCH_PIN_IRQ    80u
#define TOUCH_PIN_AVDD   36u
#define TOUCH_GPIO_CFG(n) ((uint32_t)(n) * 0x1000u)
#define TOUCH_GPIO_IO(n)  (TOUCH_GPIO_CFG(n) + 4u)
#define TOUCH_MAX        10u
#define PANEL_W          1080u
#define PANEL_H          2400u

#define SE_STATUS       0x040u
#define SE_MCLK         0x048u
#define SE_FW           0x068u
#define SE_CLK_SEL      0x07cu
#define SE_DMA          0x258u
#define SE_TX_LEN       0x26cu
#define SE_RX_LEN       0x270u
#define SE_SCL          0x278u
#define SE_CMD          0x600u
#define SE_CTRL         0x604u
#define SE_IRQ          0x610u
#define SE_IRQ_EN       0x614u
#define SE_IRQ_CLR      0x618u
#define SE_TX           0x700u
#define SE_RX           0x780u
#define SE_TX_COUNT     0x800u
#define SE_RX_COUNT     0x804u
#define SE_HW_TX        0xe24u
#define SE_HW_RX        0xe28u
#define SE_TX_WM        0x80cu
#define SE_RX_WM_IRQ    (1u << 26)
#define SE_RX_LAST      (1u << 27)
#define SE_TX_WM_IRQ    (1u << 30)
#define SE_DONE         (1u << 0)
#define SE_NACK         (1u << 10)
#define SE_ERRORS       (0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | \
                         (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS       (SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
/* geni_se_config_packing(8, 4, true, true, true): start bits 7/15/23/31,
 * MSB-to-LSB, length 8, STOP only on vector 3. FIFO bytes remain LSB first. */
#define SE_PACK0        0x0007f8feu
#define SE_PACK1        0x000ffefeu

static struct {
	vm_offset_t se, gcc, west, east;
	bool started, dead, probing;
	int irq_hint;
	uint32_t irq_status, irq_last, active;
	unsigned sent, got;
	int x[TOUCH_MAX], y[TOUCH_MAX];
	unsigned fbits, rx, ry, rawlog;
	unsigned s_frames, s_errs, s_down, s_up;
} st;
static int touch_sleep_chan;

/* Frames of the IC that are the same for too long with a contact down are a stuck one (iOS 16 port 0300, 0314). */
static unsigned touch_stale_ms = 6000;
static unsigned touch_quiet_ms = 30;      /* held fingers: IRQ gaps of 2-4 ms seen; most lifts end this way */
static uint64_t touch_irq_last;           /* when the IRQ line was last seen low */
static unsigned touch_fast = 1;           /* 0313: with a finger down, read as soon as GPIO80 says there is a frame */
static unsigned touch_delay_s = 45;

static uint32_t
touch_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
touch_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
touch_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&touch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
touch_wait_until(uint64_t deadline)
{
	(void)assert_wait_deadline(&touch_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
touch_wait_us(unsigned us)
{
	uint64_t deadline;
	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	touch_wait_until(deadline);
}

static bool
touch_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(touch_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* topaz: GPIO 36 is on the EAST tile, the touch pins otherwise on WEST. */
static vm_offset_t
touch_tile(unsigned pin)
{
	return pin == TOUCH_PIN_AVDD ? st.east : st.west;
}

static void
touch_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out, bool high)
{
	vm_offset_t tile = touch_tile(pin);
	uint32_t off = TOUCH_GPIO_CFG(pin);
	uint32_t cfg = touch_rd(tile, off);
	if (out) {
		/* Set the desired latch before enabling output (reset is active low). */
		touch_wr(tile, TOUCH_GPIO_IO(pin),
		    (touch_rd(tile, TOUCH_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	touch_wr(tile, off, cfg);
}

static bool
touch_clocks(void)
{
	/*
	 * topaz (TopazTouch hwio.c): OR the QUP0 wrapper/SE2 votes into the
	 * shared vote register (other bits belong to other clients), XO parent
	 * with divider 1 = 19.2 MHz, no PLL or voltage change.
	 */
	touch_wr(st.gcc, TOUCH_GCC_VOTE, touch_rd(st.gcc, TOUCH_GCC_VOTE) | 0x13c0u);
	printf("ft5452: gcc vote=%#x rcg=%#x cfg=%#x cbcr=%#x\n",
	    touch_rd(st.gcc, TOUCH_GCC_VOTE), touch_rd(st.gcc, TOUCH_RCG),
	    touch_rd(st.gcc, TOUCH_RCG + 4u), touch_rd(st.gcc, TOUCH_RCG - 4u));
	touch_wr(st.gcc, TOUCH_RCG + 4u, 1u);
	touch_wr(st.gcc, TOUCH_RCG, touch_rd(st.gcc, TOUCH_RCG) | 1u);
	if (!touch_wait_clear(st.gcc, TOUCH_RCG, 1u)) {
		printf("ft5452: RCG update timeout\n");
		return false;
	}
	if (!touch_wait_clear(st.gcc, TOUCH_RCG - 4u, 1u << 31)) {
		printf("ft5452: SE2 branch halted (%#x)\n", touch_rd(st.gcc, TOUCH_RCG - 4u));
		return false;
	}
	return true;
}

/* Called only for a transfer started by this driver, including a stretched TX. */

static void
touch_abort(void)
{
	/* i2c-qcom-geni.c: M_GENI_CMD_CANCEL, then ABORT if cancel does not finish */
	bool done = false;
	touch_wr(st.se, SE_CTRL, 4);
	for (unsigned i = 0; i < 1000; ++i) {
		if (touch_rd(st.se, SE_IRQ) & (1u << 4)) {
			done = true;
			break;
		}
		delay(1);
	}
	if (!done) {
		touch_wr(st.se, SE_CTRL, 2); /* abort M command, release held bus */
		for (unsigned i = 0; i < 1000; ++i) {
			if (touch_rd(st.se, SE_IRQ) & (1u << 5)) {
				done = true;
				break;
			}
			delay(1);
		}
	}
	if (!done || !touch_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true;
		printf("ft5452: abort failed status=%#x irq=%#x; stopping\n",
		    touch_rd(st.se, SE_STATUS), touch_rd(st.se, SE_IRQ));
	}
	/* Bound drain even if the hardware reports an impossible FIFO count. */
	for (unsigned i = 0; i < 64 && (touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu); ++i) {
		(void)touch_rd(st.se, SE_RX);
	}
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	if (touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) {
		st.dead = true;
		printf("ft5452: TX FIFO not empty after abort; stopping\n");
	}
}

/* Keep all observed bits for diagnostics, even if a watermark was cleared. */
static uint32_t
touch_irq(void)
{
	st.irq_last = touch_rd(st.se, SE_IRQ);
	st.irq_status |= st.irq_last;
	return st.irq_last;
}

/* One Linux-style FIFO message. TX is at most 3 bytes; RX at most 90. */
static int
touch_msg_run(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	if (st.dead || nt > 3 || nr > 90 || (nt && nr) || (!nt && !nr)) {
		return EIO;
	}
	if (!touch_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true; /* no other SE7 owner is supported */
		return EBUSY;
	}
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	touch_wr(st.se, SE_TX_WM, nt ? 1u : 0u);
	touch_wr(st.se, nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	touch_wr(st.se, SE_CMD, ((nr ? 2u : 1u) << 27) |
	    ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	if (nr > 8 && !st.probing) {
		/*
		 * Sleep, do not spin, through most of the transfer: ~22 us a byte at 400 kHz. It was
		 * a flat 2 ms -- for the 10 bytes of one contact, 0.25 ms of bus -- in every poll.
		 */
		touch_wait_us(nr * 18u);
	}
	int err = ETIMEDOUT;
	for (unsigned i = 0; i < 5000; ++i) {
		uint32_t irq = touch_irq();
		if (irq & SE_ERRORS) {
			err = (irq & SE_NACK) ? ENXIO : EIO;
			/* A completed NACK is normal during address probing. Aborting an
			 * idle sequencer may never produce ABORT_DONE. Wait for completion
			 * and leave a clean idle engine available for the other IC. */
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); ++j) {
				delay(1);
				irq = touch_irq();
			}
			if ((irq & SE_DONE) && !(touch_rd(st.se, SE_STATUS) & 1u) &&
			    !(touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) &&
			    !(touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
				touch_wr(st.se, SE_IRQ_CLR, irq);
				return err;
			}
			break;
		}
		if (nt && !st.sent && (irq & SE_TX_WM_IRQ)) {
			uint32_t word = 0;
			for (unsigned j = 0; j < nt; ++j) {
				word |= (uint32_t)tx[j] << (8u * j);
			}
			touch_wr(st.se, SE_TX, word);
			st.sent = nt;
			touch_wr(st.se, SE_TX_WM, 0);
		}
		uint32_t count = touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu;
		if (count > 64 || count > (nr - st.got + 3u) / 4u) {
			err = EIO;
			break;
		}
		while (count--) {
			uint32_t word = touch_rd(st.se, SE_RX);
			for (unsigned j = 0; j < 4 && st.got < nr; ++j) {
				rx[st.got++] = (uint8_t)(word >> (8u * j));
			}
		}
		/* DONE is checked after draining RX, so final short words aren't lost. */
		if (irq & SE_DONE) {
			/* Re-sample after the final drain; a late error must beat DONE. */
			irq = touch_irq();
			if (irq & SE_ERRORS) {
				continue;
			}
			touch_wr(st.se, SE_IRQ_CLR, irq);
			if (st.got == nr && st.sent == nt) {
				return 0;
			}
			err = EIO;
			break;
		}
		/* Preserve DONE/errors if they arrive during this iteration. */
		touch_wr(st.se, SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		delay(1);
	}
	touch_abort();
	return err;
}

static int
touch_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	st.irq_status = st.irq_last = 0;
	st.sent = st.got = 0;
	int err = touch_msg_run(addr, tx, nt, rx, nr, stretch);
	if (st.probing) {
		printf("ft5452: probe xfer addr=%#x %s len=%u stretch=%u err=%d irq=%#x seen=%#x tx=%u rx=%u status=%#x\n",
		    addr, nr ? "read" : "write", nr ? nr : nt, stretch ? 1u : 0u,
		    err, st.irq_last, st.irq_status, st.sent, st.got,
		    touch_rd(st.se, SE_STATUS));
	}
	return err;
}

static int
touch_read(uint8_t addr, uint16_t reg, bool wide, uint8_t *buf, unsigned len)
{
	uint8_t ptr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
	int err = touch_msg(addr, wide ? ptr : ptr + 1, wide ? 2u : 1u,
	    NULL, 0, true);
	if (err) {
		return err;
	}
	/* STOP_STRETCH on the pointer write retains the bus for a repeated START. */
	return touch_msg(addr, NULL, 0, buf, len, false);
}



static bool
touch_se_init(void)
{
	uint32_t fw = touch_rd(st.se, SE_FW);
	uint32_t tx = touch_rd(st.se, SE_HW_TX), rx = touch_rd(st.se, SE_HW_RX);
	printf("ft5452: se2 fw=%#x proto=%u status=%#x tx=%#x rx=%#x ios=%#x\n",
	    fw, (fw >> 8) & 0xffu, touch_rd(st.se, SE_STATUS), tx, rx,
	    touch_rd(st.se, 0x908));
	if (((fw >> 8) & 0xffu) != 3u || (touch_rd(st.se, 0x64) & 1u) ||
	    ((tx >> 24) & 0x3fu) != 32u || ((rx >> 24) & 0x3fu) != 32u ||
	    ((tx >> 16) & 0x3fu) < 2u || ((rx >> 16) & 0x3fu) < 2u) {
		printf("ft5452: need ABL-loaded I2C firmware and 32-bit FIFOs; probes skipped\n");
		return false;
	}
	if (touch_rd(st.se, SE_STATUS) & 0x1001u) {
		printf("ft5452: SE2 already busy; probes skipped\n");
		return false;
	}
	touch_wr(st.se, 0xe18, 0); /* no GSI events */
	touch_wr(st.se, 0xe1c, 0); /* mask external IRQ routing; poll local status */
	touch_wr(st.se, 0x644, 0);
	touch_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	touch_wr(st.se, 0x648, 0xffffffffu);
	touch_wr(st.se, SE_DMA, 0);
	touch_wr(st.se, 0x28, touch_rd(st.se, 0x28) | 0x7fu);
	touch_wr(st.se, 0xe30, touch_rd(st.se, 0xe30) | 0x0fu);
	touch_wr(st.se, 0x24, 0x7f);
	touch_wr(st.se, 0x20, 1);
	touch_wr(st.se, SE_CLK_SEL, 0);
	touch_wr(st.se, SE_MCLK, (2u << 4) | 1u);
	touch_wr(st.se, SE_SCL, (5u << 20) | (11u << 10) | 22u); /* 400 kHz */
	/* Linux 8-bit I2C packing: MSB on the wire, LSB byte first in FIFO. */
	touch_wr(st.se, 0x254, 0);
	touch_wr(st.se, 0x260, SE_PACK0);
	touch_wr(st.se, 0x264, SE_PACK1);
	touch_wr(st.se, 0x284, SE_PACK0);
	touch_wr(st.se, 0x288, SE_PACK1);
	touch_wr(st.se, SE_TX_WM, 0);
	touch_wr(st.se, 0x810, 0);
	touch_wr(st.se, 0x814, ((rx >> 16) & 0x3fu) - 1u);
	touch_wr(st.se, SE_IRQ_EN, SE_EVENTS);
	printf("ft5452: packing tx=%#x/%#x rx=%#x/%#x gran=%#x\n",
	    touch_rd(st.se, 0x260), touch_rd(st.se, 0x264),
	    touch_rd(st.se, 0x284), touch_rd(st.se, 0x288), touch_rd(st.se, 0x254));
	if (touch_rd(st.se, 0x260) != SE_PACK0 || touch_rd(st.se, 0x264) != SE_PACK1 ||
	    touch_rd(st.se, 0x284) != SE_PACK0 || touch_rd(st.se, 0x288) != SE_PACK1 ||
	    touch_rd(st.se, 0x254) != 0) {
		printf("ft5452: packing readback mismatch; probes skipped\n");
		return false;
	}
	if ((touch_rd(st.se, SE_TX_COUNT) & 0xfffffffu) ||
	    (touch_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
		printf("ft5452: inherited FIFO data; probes skipped\n");
		return false;
	}
	return true;
}


/* One report's contacts to the queue: down the first time, move when the position changed, up when gone. */
static void
touch_report(uint32_t mask, const int *x, const int *y)
{
	for (unsigned id = 0; id < TOUCH_MAX; ++id) {
		uint32_t bit = 1u << id;

		if (mask & bit) {
			if (!(st.active & bit) || x[id] != st.x[id] || y[id] != st.y[id]) {
				uint32_t nx = (uint32_t)x[id] * 65535u / (PANEL_W - 1u);
				uint32_t ny = (uint32_t)y[id] * 65535u / (PANEL_H - 1u);

				redwood_touch_post(!(st.active & bit) ? REDWOOD_TOUCH_DOWN : REDWOOD_TOUCH_MOVE, id, nx, ny);
				st.s_down++;
			}
			st.x[id] = x[id];
			st.y[id] = y[id];
		} else if (st.active & bit) {
			uint32_t nx = (uint32_t)st.x[id] * 65535u / (PANEL_W - 1u);
			uint32_t ny = (uint32_t)st.y[id] * 65535u / (PANEL_H - 1u);

			redwood_touch_post(REDWOOD_TOUCH_UP, id, nx, ny);
			st.s_up++;
		}
	}
	st.active = mask;
}

static int
touch_focaltech(void)
{
	uint8_t buf[63];
	int x[TOUCH_MAX] = {0}, y[TOUCH_MAX] = {0};
	uint32_t mask = 0, seen = 0;
	int err = touch_read(0x38, 0, false, buf, sizeof(buf));

	if (err) {
		return err;
	}
	/* a frame repeated for too long with a contact down is a stuck one */
	static uint8_t last[sizeof(buf)];
	static uint64_t same_since;
	static bool stale;
	uint64_t now = mach_absolute_time();

	if (memcmp(buf, last, sizeof(buf)) != 0) {
		memcpy(last, buf, sizeof(buf));
		same_since = now;
		stale = false;
	} else if (stale) {
		return 0;               /* the stuck frame again: nothing new */
	} else if (st.active) {
		uint64_t ns, gns, gap = touch_irq_last != 0 && now > touch_irq_last ? now - touch_irq_last : 0;

		absolutetime_to_nanoseconds(now - same_since, &ns);
		absolutetime_to_nanoseconds(gap, &gns);
		unsigned ms = (unsigned)(ns / NSEC_PER_MSEC), quiet = (unsigned)(gns / NSEC_PER_MSEC);

		/* the IC quiet as well as the frame the same: with a finger on the glass it keeps interrupting */
		if (st.irq_hint && touch_quiet_ms != 0 && touch_irq_last != 0 && ms >= touch_quiet_ms &&
		    quiet >= touch_quiet_ms) {
			stale = true;
			printf("ft5452: same frame for %u ms and no IRQ for %u ms with contacts %#x down: released (IC quiet)\n",
			    ms, quiet, st.active);
			touch_report(0, x, y);
			return 0;
		}
		if (touch_stale_ms != 0 && ms >= touch_stale_ms) {
			stale = true;
			printf("ft5452: one frame for %u ms with contacts %#x down: released (stuck)\n", ms, st.active);
			touch_report(0, x, y);
			return 0;
		}
	} else {
		same_since = now;
	}
	unsigned n = buf[2] & 15u;

	st.s_frames++;
	if (n > TOUCH_MAX && buf[3] == 0xffu) {
		/* no contact: the IC (firmware as updated by HyperOS) reports TD_STATUS 0xff and 0xff records: release all */
		touch_report(0, x, y);
		return 0;
	}
	if (n > TOUCH_MAX) {
		static uint64_t last_dump;
		uint64_t gap;

		nanoseconds_to_absolutetime(NSEC_PER_SEC, &gap);
		if (now - last_dump > gap) {
			last_dump = now;
			printf("ft5452: bad frame n=%u raw %02x %02x %02x | %02x %02x %02x %02x %02x %02x\n",
			    n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8]);
		}
		return EIO;
	}
	for (unsigned i = 0; i < TOUCH_MAX; ++i) {
		const uint8_t *p = buf + 3u + 6u * i;
		unsigned id = p[2] >> 4, event = p[0] >> 6;

		if (id >= TOUCH_MAX) {
			break; /* vendor's unused-record sentinel */
		}
		if (seen & (1u << id)) {
			return EIO;
		}
		seen |= 1u << id;
		if (event == 1u || event == 3u) {
			continue; /* an UP record, or a "no event" slot */
		}
		unsigned hx = ((unsigned)(p[0] & 15u) << 8) | p[1];
		unsigned hy = ((unsigned)(p[2] & 15u) << 8) | p[3];
		uint64_t fx, fy;

		/*
		 * Raw position = 12-bit coordinate plus st.fbits fraction bits from byte 4: 2 bits each (x 7:6,
		 * y 5:4), or 4 bits each (x 7:4, y 3:0) on hi-res firmware. st.rx/st.ry map the raw range onto 1080x2400.
		 */
		if (st.fbits == 4u) {
			fx = (uint64_t)hx << 4 | (p[4] >> 4);
			fy = (uint64_t)hy << 4 | (p[4] & 15u);
		} else {
			fx = (uint64_t)hx << 2 | (p[4] >> 6);
			fy = (uint64_t)hy << 2 | ((p[4] >> 4) & 3u);
		}
		unsigned px = (unsigned)(fx * PANEL_W / st.rx);
		unsigned py = (unsigned)(fy * PANEL_H / st.ry);

		if (st.rawlog && !(st.active & (1u << id))) {
			st.rawlog--;
			printf("ft5452: raw id %u x %u.%x y %u.%x -> %u,%u\n", id, hx, p[4] >> 4, hy, p[4] & 15u, px, py);
		}
		px = px > PANEL_W - 1u ? PANEL_W - 1u : px;
		py = py > PANEL_H - 1u ? PANEL_H - 1u : py;
		mask |= 1u << id;
		x[id] = (int)px;
		y[id] = (int)py;
	}
	touch_report(mask, x, y);
	return 0;
}

static void
touch_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	/* Start late: if the bus access below hangs the SoC, the persistent log and the GUI got there first. */
	printf("ft5452: starting in %u s\n", touch_delay_s);
	touch_wait(touch_delay_s * 1000u);
	printf("ft5452: mapping GCC/TLMM/SE2\n");
	st.gcc = ml_io_map(TOUCH_GCC_PA, 0x80000);
	st.west = ml_io_map(TOUCH_WEST_PA, 0x58000);
	st.east = ml_io_map(TOUCH_EAST_PA, 0x25000);
	st.se = ml_io_map(TOUCH_SE_PA, 0x4000);
	if (!st.gcc || !st.west || !st.east || !st.se) {
		printf("ft5452: MMIO mapping failed\n");
		goto out;
	}
	if (!touch_clocks() || !touch_se_init()) {
		goto out;
	}
	printf("ft5452: TLMM reset=%#x irq=%#x avdd=%#x\n",
	    touch_rd(st.west, TOUCH_GPIO_IO(TOUCH_PIN_RESET)),
	    touch_rd(st.west, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)),
	    touch_rd(st.east, TOUCH_GPIO_IO(TOUCH_PIN_AVDD)));
	/* IRQ input, AVDD on, I2C pins, then the reset pulse. */
	touch_gpio(TOUCH_PIN_IRQ, 0, 3, 3, false, false);       /* CTL 0xc3 */
	touch_gpio(TOUCH_PIN_AVDD, 0, 3, 3, true, true);        /* CTL 0x2c3, high */
	touch_gpio(6, 1, 0, 0, false, false);                   /* qup2 SDA, CTL 0x4 */
	touch_gpio(7, 1, 0, 0, false, false);                   /* qup2 SCL */
	touch_gpio(TOUCH_PIN_RESET, 0, 3, 3, true, false);      /* CTL 0x2c3, low */
	touch_wait(20);
	touch_gpio(TOUCH_PIN_RESET, 0, 3, 3, true, true);
	touch_wait(250);
	uint8_t fid1 = 0, fid2 = 0, fw = 0, vendor = 0;

	st.probing = true;
	int fe1 = touch_read(0x38, 0xa3, false, &fid1, 1);
	int fe2 = touch_read(0x38, 0x9f, false, &fid2, 1);
	int e1 = touch_read(0x38, 0xa6, false, &fw, 1);
	int e2 = touch_read(0x38, 0xa8, false, &vendor, 1);
	st.probing = false;
	printf("ft5452: probe 0x38 err=%d/%d chip-id=%02x:%02x fw=%02x/%d vendor=%02x/%d irq=%#x\n",
	    fe1, fe2, fid1, fid2, fw, e1, vendor, e2, st.irq_status);
	if (st.dead || fe1 || fe2 || fid1 != 0x54 || fid2 != 0x52) {
		printf("ft5452: no FocalTech 54:52 at 0x38 (dead=%d); stopping\n", st.dead);
		goto out;
	}
	printf("ft5452: selected Focaltech 54:52, %ux%u, irq-hint=%d\n", PANEL_W, PANEL_H, st.irq_hint);
	unsigned errors = 0, ticks = 0;
	uint64_t next = 0, period_down, period_up, period_fast, late_after, last_read = 0;

	/* 250 Hz on an absolute grid while a finger is down, 10 ms with none (iOS 16 port 0285) */
	nanoseconds_to_absolutetime(4 * NSEC_PER_MSEC, &period_down);
	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &period_up);
	nanoseconds_to_absolutetime(500 * NSEC_PER_USEC, &period_fast);
	nanoseconds_to_absolutetime(6 * NSEC_PER_MSEC, &late_after);
	while (!st.dead) {
		bool fast = touch_fast && st.irq_hint && st.active;
		bool irq_low = st.irq_hint && !(touch_rd(st.west, TOUCH_GPIO_IO(TOUCH_PIN_IRQ)) & 1u);
		bool want;

		if (irq_low) {
			touch_irq_last = mach_absolute_time();
		}
		if (fast) {
			/* as soon as the IC has a frame, or 6 ms after the last read anyway (0313) */
			want = irq_low || mach_absolute_time() - last_read >= late_after;
		} else {
			want = !st.irq_hint || st.active || irq_low || (++ticks % 10u) == 0;
		}
		if (want) {
			last_read = mach_absolute_time();
			int err = touch_focaltech();

			if (err) {
				st.s_errs++;
				if ((errors++ % 100u) == 0) {
					printf("ft5452: report err=%d irq=%#x\n", err, st.irq_status);
				}
				if (errors == 10) {
					touch_report(0, st.x, st.y); /* release after persistent bad reads */
				}
			} else {
				errors = 0;
			}
		}
		uint64_t period = st.active ? (fast ? period_fast : period_down) : period_up, now = mach_absolute_time();

		next = (next == 0 || now > next + period) ? now + period : next + period;
		touch_wait_until(next);
	}
	touch_report(0, st.x, st.y);
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
redwood_ft5452_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (st.started || !PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || enabled != 2) {
		return;
	}
	st.started = true;
	st.irq_hint = 1;        /* read on IRQ GPIO80 low, while touching, else every 100 ms */
	st.fbits = 2;
	st.rx = 8640;           /* sapphire: X 0..2159, Y 0..2399, two fraction bits */
	st.ry = 9600;
	st.rawlog = 40;
	(void)PE_parse_boot_argn("touch_irq", &st.irq_hint, sizeof(st.irq_hint));
	(void)PE_parse_boot_argn("touch_fast", &touch_fast, sizeof(touch_fast));
	(void)PE_parse_boot_argn("touch_delay", &touch_delay_s, sizeof(touch_delay_s));
	(void)PE_parse_boot_argn("touch_quiet_ms", &touch_quiet_ms, sizeof(touch_quiet_ms));
	(void)PE_parse_boot_argn("touch_stale_ms", &touch_stale_ms, sizeof(touch_stale_ms));
	(void)PE_parse_boot_argn("touch_fbits", &st.fbits, sizeof(st.fbits));
	(void)PE_parse_boot_argn("touch_rx", &st.rx, sizeof(st.rx));
	(void)PE_parse_boot_argn("touch_ry", &st.ry, sizeof(st.ry));
	(void)PE_parse_boot_argn("touch_rawlog", &st.rawlog, sizeof(st.rawlog));
	if (st.fbits != 4u) {
		st.fbits = 2;
	}
	if (!st.rx || !st.ry) {
		st.rx = 8640;
		st.ry = 9600;
	}
	printf("ft5452: raw range %ux%u, %u fraction bits\n", st.rx, st.ry, st.fbits);
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("ft5452: kernel_thread_start failed\n");
	}
}
