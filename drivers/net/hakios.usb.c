/*
 * The USB device port of the Redmi Note 13 4G (sapphire, SM6225 / khaje) as the link behind a network
 * interface (CDC-ECM, unit 2: en2, 192.168.7.2 on the cable side by hand or by the host's DHCP). Ported
 * from the iOS 16 port's bsd/dev/sweet_usb.c (xnu/patches 0072-0077, 0142, 0159, 0162, 0224, 0284,
 * 0289-0291), which has run on the phone: the DWC3 engine -- control endpoint 0, the bulk and interrupt
 * endpoints, bus reset, the watch for transfers whose event never came -- is its own; what changed is the
 * way to the network stack: not an interface of its own (ue0, and a UDP log) but a struct hakios_net_link
 * that drivers/net/hakios.net.c turns into the interface, as for the radios. Boot-arg usb=1 reports the
 * state ABL left, usb=2 also starts the device.
 *
 * SWEET: USB bring-up, step 1 - read-only state probe.
 *
 * The phone's USB is a Synopsys DWC3 (USB 2.0 only, maximum-speed
 * high-speed) behind Qualcomm's dwc-usb3-msm wrapper; its power domain is
 * the USB30_PRIM GDSC and its clocks come from GCC. sapphire/topaz
 * (SM6225 khaje): DWC3 at 0x04e00000, Synopsys femto HS PHY at 0x01613000
 * (not read here), GCC offsets from Linux gcc-sm6115.c (the khaje GCC; the
 * USB30_PRIM GDSC at 0x141a004 matches the phone's DT). ABL uses this
 * controller for fastboot; this probe only reports what state it is left
 * in after `fastboot boot`. It never writes a register, and it touches the
 * DWC3 / PHY register blocks only when the GCC status registers say their
 * power domain and clocks are on (an access to a gated block would hang).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <sys/hakios.net.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <sys/sysctl.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void hakios_net_input_unit(unsigned unit, const uint8_t *frame, size_t length);
extern void hakios_net_link_state_unit(unsigned unit, bool up);
void hakios_usb_init(void);
static void hakios_usb_gadget_start(vm_offset_t dwc);
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);

#define GCC_PA                  0x01400000
#define GCC_SIZE                0x001f0000
#define GCC_USB30_PRIM_GDSCR    0x1a004         /* bit 31: power on */
#define CBCR_CLK_OFF            (1u << 31)

static const struct {
	const char *name;
	uint32_t    off;
} usb_cbcr[] = {
	{ "master", 0x1a010 },
	{ "sleep", 0x1a014 },
	{ "utmi", 0x1a018 },
	{ "cfg-noc-axi", 0x1a084 },
	{ "sys-noc-axi", 0x1a080 },
	{ "ahb2phy", 0x1d008 },
	{ "clkref", 0x9f000 },
};

#define DWC3_PA                 0x04e00000
#define DWC3_SIZE               0x0000d000

static uint32_t
usb_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
hakios_usb_init(void)
{
	int enabled = 0;
	uint32_t gdscr, cbcr[sizeof(usb_cbcr) / sizeof(usb_cbcr[0])];
	bool clocks_on = true;

	/* sweetusb=0 (default) off, 1 report state only, 2 also start the USB device */
	PE_parse_boot_argn("usb", &enabled, sizeof(enabled));
	if (!enabled) {
		return;
	}
	vm_offset_t gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		printf("usb: GCC map failed\n");
		return;
	}
	gdscr = usb_rd(gcc, GCC_USB30_PRIM_GDSCR);
	printf("usb: GDSC usb30_prim 0x%08x (%s)\n", gdscr, (gdscr & (1u << 31)) ? "on" : "off");
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		cbcr[i] = usb_rd(gcc, usb_cbcr[i].off);
		printf("usb: clk %-12s CBCR 0x%08x (%s)\n", usb_cbcr[i].name, cbcr[i],
		    (cbcr[i] & CBCR_CLK_OFF) ? "off" : "on");
	}
	/* DWC3 registers need the GDSC, the master clock and the config-NoC path. */
	clocks_on = (gdscr & (1u << 31)) && !(cbcr[0] & CBCR_CLK_OFF) && !(cbcr[3] & CBCR_CLK_OFF);
	if (!clocks_on) {
		printf("usb: controller unpowered or unclocked; DWC3 not read\n");
		return;
	}
	vm_offset_t dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
	if (dwc == 0) {
		printf("usb: DWC3 map failed\n");
		return;
	}
	printf("usb: DWC3 GSNPSID 0x%08x GCTL 0x%08x GSTS 0x%08x GUSB2PHYCFG 0x%08x\n",
	    usb_rd(dwc, 0xc120), usb_rd(dwc, 0xc110), usb_rd(dwc, 0xc118), usb_rd(dwc, 0xc200));
	printf("usb: DWC3 DCFG 0x%08x DCTL 0x%08x DEVTEN 0x%08x DSTS 0x%08x DALEPENA 0x%08x\n",
	    usb_rd(dwc, 0xc700), usb_rd(dwc, 0xc704), usb_rd(dwc, 0xc708), usb_rd(dwc, 0xc70c),
	    usb_rd(dwc, 0xc720));
	printf("usb: DWC3 GEVNTADR 0x%08x%08x GEVNTSIZ 0x%08x GEVNTCOUNT 0x%08x GHWPARAMS0 0x%08x\n",
	    usb_rd(dwc, 0xc404), usb_rd(dwc, 0xc400), usb_rd(dwc, 0xc408), usb_rd(dwc, 0xc40c),
	    usb_rd(dwc, 0xc140));
	if (enabled >= 2) {
		hakios_usb_gadget_start(dwc);
	}
}

/*
 * Step 2: USB device mode with only the default control endpoint, enough
 * for the host to enumerate the phone (boot-arg usb=2). ABL leaves
 * the DWC3 powered, clocked, in device mode and halted, with the PHY set
 * up, so only the device controller is restarted: soft reset, an event
 * buffer in our memory, endpoint 0, run. Events are polled from a kernel
 * thread (no interrupt), like the UFS driver. DMA is not cache coherent
 * (DT qcom,gsi-disable-io-coherency): buffers are cleaned/invalidated.
 * Register layout and the control-transfer state machine follow the
 * Synopsys DWC3 databook as used by Linux drivers/usb/dwc3 (gadget.c,
 * ep0.c).
 */
#define GCTL            0xc110
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEPCMDPAR2(n)   (0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)   (0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)   (0xc808 + 0x10 * (n))
#define DEPCMD(n)       (0xc80c + 0x10 * (n))

/*
 * Qualcomm wrapper registers ("QSCRATCH", core base + 0xf8800). The
 * controller only connects (pull-up on D+) while it sees a valid VBUS
 * session; Linux dwc3-qcom forces that in software when the charger
 * reports a cable (dwc3_qcom_vbus_override_enable).
 */
#define QSCRATCH_PA             0x04ef8800    /* DWC3 + 0xf8800 */
#define QSCRATCH_HS_PHY_CTRL    0x10
#define   UTMI_OTG_VBUS_VALID   (1u << 20)
#define   SW_SESSVLD_SEL        (1u << 28)
#define QSCRATCH_SS_PHY_CTRL    0x30
#define   LANE0_PWR_PRESENT     (1u << 24)

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)

#define DEPCMD_SETEPCONFIG      1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL         4
#define DEPCMD_CLEARSTALL       5
#define DEPCMD_STARTTRANSFER    6
#define DEPCMD_ENDTRANSFER      8
#define DEPCMD_HIPRI_FORCERM    (1u << 11)
#define DEPCMD_DEPSTARTCFG      9
#define DEPCMD_CMDACT           (1u << 10)

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

/* DMA area layout */
#define UD_EVT          0x0000          /* event buffer, 4 KiB */
#define UD_EVT_SIZE     0x1000
#define UD_TRB_OUT      0x1000          /* ep0 OUT TRB */
#define UD_TRB_IN       0x1010          /* ep0 IN TRB */
#define UD_TRB_RX       0x1020          /* ep1 OUT (bulk, host -> phone) */
#define UD_TRB_TX       0x1030          /* ep1 IN (bulk, phone -> host) */
#define UD_TRB_INT      0x1050          /* ep2 IN (interrupt, notifications) */
#define UD_SETUP        0x1040          /* 8-byte setup packet */
#define UD_DATA         0x1100          /* ep0 data stage, 512 bytes */
#define UD_NOTIFY       0x1300          /* 16-byte notification */
#define UD_RX           0x2000          /* one received frame */
#define UD_TX           0x2800          /* one frame to send */
#define UD_FRAME_MAX    0x800
#define UD_SIZE         0x4000

/* Physical endpoints: 2n = OUT, 2n+1 = IN */
#define EP_RX           2               /* 0x01 bulk OUT */
#define EP_TX           3               /* 0x81 bulk IN */
#define EP_INT          5               /* 0x82 interrupt IN */
#define TRBCTL_NORMAL   (1u << 4)

#define UD_TXQ          32
#define UD_NET_UNIT     2               /* en2: 0 is the Wi-Fi, 1 the modem */
#define UD_TXQ_FRAME    1536
static uint8_t ud_txq_frames[UD_TXQ][UD_TXQ_FRAME];

static struct {
	vm_offset_t dwc;
	vm_offset_t dma;
	uint64_t    dma_pa;
	uint32_t    evt_pos;
	int         sleep_chan;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	bool        data_in;        /* data stage direction */
	uint32_t    events;
	bool        configured;
	uint8_t     data_alt;       /* data interface alternate setting */
	/* network */
	bool        linked;
	lck_mtx_t  *lock;           /* TX ring and TX endpoint */
	uint16_t    txlen[UD_TXQ];
	unsigned    txq_head, txq_count;
	bool        tx_busy;
	uint64_t    rx_frames, tx_frames;
	/* every endpoint's transfer (control 0/1 and data), to end them on a bus reset */
	bool        xfer_active[EP_INT + 1];
	uint8_t     xfer_rsc[EP_INT + 1];
	uint32_t    resets;
	/* transfers the controller finished without an event reaching us (ud_watch) */
	uint32_t    lost_rx, lost_tx, stale_rx;
	uint32_t    rx_rearms;      /* RX armed but silent, ended and armed again (ud_watch) */
	uint32_t    halt_clears;    /* 0289: CLEAR_FEATURE(ENDPOINT_HALT) / SET_INTERFACE: sequence numbers reset */
	uint64_t    watch_rx, watch_tx;
} ud;

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
dwc_rd(uint32_t off)
{
	return usb_rd(ud.dwc, off);
}

static void
ud_sync(uint32_t off, uint32_t len)
{
	flush_dcache64((addr64_t)(ud.dma_pa + off), len, TRUE);
}

static void
ud_sleep_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ud.sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			uint32_t status = (v >> 12) & 0xf;
			if (status) {
				printf("usb: ep%u cmd 0x%x status %u\n", ep, cmd & 0xf, status);
				return -1;
			}
			return 0;
		}
		delay(1);
	}
	printf("usb: ep%u cmd 0x%x timeout\n", ep, cmd & 0xf);
	return -1;
}

/* One TRB (at DMA offset trb) on physical endpoint ep: 0, or -1 if the controller refused it. */
static int
ud_start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	ud_sync(trb, 16);
	if (len) {
		ud_sync(buf, len);
	}
	uint64_t tpa = ud.dma_pa + trb;
	if (ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0) != 0) {
		return -1;
	}
	if (ep <= EP_INT) {
		/* the transfer resource index, for ENDTRANSFER (Linux: dwc3_gadget_ep_get_transfer_index) */
		ud.xfer_rsc[ep] = (uint8_t)((dwc_rd(DEPCMD(ep)) >> 16) & 0x7f);
		ud.xfer_active[ep] = true;
	}
	return 0;
}

static int
ud_start(unsigned ep, uint32_t buf, uint32_t len, uint32_t ctl)
{
	return ud_start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, buf, len, ctl);
}

/* End endpoint ep's transfer, if it has one. */
static void
ud_end_transfer(unsigned ep)
{
	if (ep <= EP_INT && ud.xfer_active[ep]) {
		ud_epcmd(ep, DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | ((uint32_t)ud.xfer_rsc[ep] << 16), 0, 0, 0);
		delay(100);
		ud.xfer_active[ep] = false;
	}
}

/*
 * Arm ep0 for the next SETUP. If the controller refuses (a control transfer the host abandoned
 * is still active: "ep0 cmd 0x6 status 1"), end it and try again: otherwise ep0 never listens
 * again, and every enumeration after that fails at the first GET_DESCRIPTOR.
 */
static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	if (ud_start(0, UD_SETUP, 8, TRBCTL_SETUP) != 0) {
		/* the transfer still active on ep0 is ended by the index it was started with */
		ud.xfer_active[0] = true;
		ud_end_transfer(0);
		ud_end_transfer(1);
		if (ud_start(0, UD_SETUP, 8, TRBCTL_SETUP) != 0) {
			printf("usb: ep0 cannot be armed for SETUP\n");
		}
	}
}

static void
ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/*
 * USB descriptors: a CDC-ECM (Ethernet) function, which Linux (cdc_ether)
 * and macOS drive without extra drivers. VID 0x1209 / PID 0x0001 =
 * pid.codes test ID, not a product.
 */
static const uint8_t ud_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t ud_cfg_desc[80] = {
	9, 2, 80, 0, 2, 1, 0, 0x80, 250,                /* config: 2 interfaces, 500 mA */
	9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,             /* comm interface 0: CDC ECM, 1 endpoint */
	5, 0x24, 0x00, 0x10, 0x01,                      /* header, CDC 1.10 */
	5, 0x24, 0x06, 0, 1,                            /* union: control 0, data 1 */
	13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0, /* ethernet: MAC string 4, max segment 1514 */
	7, 5, 0x82, 0x03, 16, 0, 9,                     /* ep 0x82 interrupt IN, 16 bytes, 2^(9-1) uframes */
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,             /* data interface 1 alt 0: no endpoints */
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,             /* data interface 1 alt 1: two bulk endpoints */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* ep 0x81 bulk IN, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                /* ep 0x01 bulk OUT, 512 */
};
/* The host side's MAC (ECM iMACAddress); the phone's own is ..:02 */
static const char *const ud_strings[] = {
	NULL, "ios-sweet", "Sweet iOS (USB Ethernet)", "0001", "02005E77EE01",
};

static uint32_t
ud_string(unsigned idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;   /* en-US */
		return 4;
	}
	if (idx >= sizeof(ud_strings) / sizeof(ud_strings[0])) {
		return 0;
	}
	const char *str = ud_strings[idx];
	uint32_t n = 2;
	for (; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void ud_rx_arm(void);
static void ud_tx_kick_locked(void);
static void ud_ep_reset_seq(unsigned ep);

/*
 * 0290: what the controller and the host did, kept for after a dead link (the console log goes
 * over the same link): 256 entries of (ms since boot, what, a, b). Read: kern.hakios_usb_trace.
 */
#define UT_N 256
static struct ut_ent { uint32_t ms; char what[4]; uint32_t a, b; } ut_ring[UT_N];
static uint32_t ut_pos;
static uint64_t ut_last_rx_ms;

static uint32_t
ut_now_ms(void)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return (uint32_t)(ns / 1000000ull);
}

static void
ut(const char *what, uint32_t a, uint32_t b)
{
	struct ut_ent *e = &ut_ring[ut_pos++ % UT_N];
	e->ms = ut_now_ms();
	strlcpy(e->what, what, sizeof(e->what));
	e->a = a;
	e->b = b;
}

/* NETWORK_CONNECTION notification: the host brings its link up on it. */
static void
ud_notify_connected(void)
{
	uint8_t *n = (uint8_t *)(ud.dma + UD_NOTIFY);
	n[0] = 0xa1; n[1] = 0x00;       /* NETWORK_CONNECTION */
	n[2] = 1; n[3] = 0;             /* connected */
	n[4] = 0; n[5] = 0;             /* interface 0 */
	n[6] = 0; n[7] = 0;
	ud_start_trb(EP_INT, UD_TRB_INT, UD_NOTIFY, 8, TRBCTL_NORMAL);
}

/* SET_CONFIGURATION(1): bulk IN/OUT and the interrupt endpoint. */
static void
ud_configure(void)
{
	if (ud.configured) {
		return;
	}
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
	    (1u << 8) | (8u << 16) | ((uint32_t)EP_INT << 25), 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("usb: data endpoints setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT));
	ud.configured = true;
	ud_rx_arm();
	if (ud.linked) {
		hakios_net_link_state_unit(UD_NET_UNIT, true);
	}
	printf("usb: ECM endpoints configured\n");
}

static void
ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	bool ok = true;

	printf("usb: setup %02x %02x %04x %04x len %u\n", type, req, value, index, length);
	ut("SET", (uint32_t)type << 8 | req, (uint32_t)value << 16 | index);
	ud.data_in = (type & 0x80) != 0;
	if ((type & 0x60) == 0x20) {    /* class request (e.g. SET_ETHERNET_PACKET_FILTER) */
		ok = (length == 0);
	} else if ((type & 0x60) != 0) {
		ok = false;
	} else if (req == 6) {          /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: memcpy(data, ud_dev_desc, sizeof(ud_dev_desc)); n = sizeof(ud_dev_desc); break;
		case 2: memcpy(data, ud_cfg_desc, sizeof(ud_cfg_desc)); n = sizeof(ud_cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = false; break;
		}
	} else if (req == 5) {          /* SET_ADDRESS: takes effect now (DWC3 handles the timing) */
		uint32_t dcfg = dwc_rd(DCFG) & ~(0x7fu << 3);
		dwc_wr(DCFG, dcfg | ((uint32_t)(value & 0x7f) << 3));
		printf("usb: address %u\n", value & 0x7f);
	} else if (req == 9) {          /* SET_CONFIGURATION */
		if (value == 1) {
			ud_configure();
		}
		printf("usb: configured (%u)\n", value);
	} else if (req == 8) {          /* GET_CONFIGURATION */
		data[0] = ud.configured ? 1 : 0; n = 1;
	} else if (req == 0) {          /* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11) {         /* SET_INTERFACE */
		if (index == 1) {
			if (value == 1 && ud.configured && ud.lock != NULL) {
				ud_ep_reset_seq(EP_RX);  /* 0289: a new alternate setting starts at DATA0 */
				ud_ep_reset_seq(EP_TX);
			}
			ud.data_alt = (uint8_t)value;
			if (value == 1 && ud.configured) {
				ud_notify_connected();
			}
		}
	} else if (req == 10) {         /* GET_INTERFACE */
		data[0] = index == 1 ? ud.data_alt : 0; n = 1;
	} else if (req == 1 && (type & 0x1f) == 2 && value == 0) { /* CLEAR_FEATURE(ENDPOINT_HALT) */
		unsigned a = index & 0xff;
		if (ud.configured && ud.lock != NULL) {
			if (a == 0x01) {
				ud_ep_reset_seq(EP_RX);
			} else if (a == 0x81) {
				ud_ep_reset_seq(EP_TX);
			} else if (a == 0x82) {
				ud_ep_reset_seq(EP_INT);
			}
		}
	} else if (req == 1 || req == 3) { /* other CLEAR/SET_FEATURE (remote wakeup; SET halt is not used here) */
	} else {
		ok = false;
	}
	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;    /* status stage on XferNotReady */
		return;
	}
	ud.state = EP0_DATA;
	if (ud.data_in) {
		if (n > length) {
			n = length;
		}
		ud_start(1, UD_DATA, n, TRBCTL_DATA);
	} else {
		ud_start(0, UD_DATA, 64, TRBCTL_DATA);  /* OUT data is not used here */
	}
}

/* ---- network interface (en2, through hakios.net.c) ---- */

/*
 * 0289: the host reset a data endpoint's data toggle (CLEAR_FEATURE(ENDPOINT_HALT), or SET_INTERFACE,
 * which resets every endpoint of the interface): the controller's sequence number must go back to
 * DATA0 as well (DWC3 ClearStall does that), its transfer ended first and armed again after.
 * Ignoring it left the bulk OUT one toggle off: every packet from the host was ACKed and thrown away
 * as a retry, the link silent until the next bus reset. usbipd/Windows reset the pipe that way after
 * a transfer it cancelled (a cdc_ether transmit timeout on the PC), which is how big copies died.
 */
static void
ud_ep_reset_seq(unsigned ep)
{
	ud.halt_clears++;
	if (ep == EP_TX) {
		lck_mtx_lock(ud.lock);
		ud_end_transfer(EP_TX);
		(void)ud_epcmd(EP_TX, DEPCMD_CLEARSTALL, 0, 0, 0);
		ud.tx_busy = false;
		ud_tx_kick_locked();
		lck_mtx_unlock(ud.lock);
	} else if (ep == EP_RX) {
		ud_end_transfer(EP_RX);
		(void)ud_epcmd(EP_RX, DEPCMD_CLEARSTALL, 0, 0, 0);
		ud_rx_arm();
	} else if (ep == EP_INT) {
		ud_end_transfer(EP_INT);
		(void)ud_epcmd(EP_INT, DEPCMD_CLEARSTALL, 0, 0, 0);
	}
}

static void
ud_rx_arm(void)
{
	/* refused (a transfer still held by the controller): end it and arm again, or RX is dead for good */
	if (ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL) != 0) {
		ut("RXR", dwc_rd(DEPCMD(EP_RX)), 0);
		ud.xfer_active[EP_RX] = true;
		ud_end_transfer(EP_RX);
		(void)ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL);
	}
}

static void
ud_rx_done(void)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
	ud_sync(UD_TRB_RX, 16);
	/*
	 * 0291: the completion event can come before the controller's write-back of the TRB is visible
	 * (HWO still set). Returning there left RX unarmed for good -- the event had already marked the
	 * transfer inactive, so ud_watch did not look at it either -- and every bulk OUT from the host
	 * was NAKed from then on: the PC -> phone direction of the link died ("late rx" went up by one
	 * each time). Wait for the write-back; if HWO stays set the controller finished the transfer
	 * without writing the TRB back (that frame is lost): arm RX again at once (ud_rx_arm ends a
	 * transfer the controller still holds first).
	 */
	for (int i = 0; (t[3] & TRB_HWO) && i < 100; i++) {
		delay(10);
		ud_sync(UD_TRB_RX, 16);
	}
	if (t[3] & TRB_HWO) {
		ud.stale_rx++;
		ut("RXS", t[3], t[2]);
		ud_rx_arm();
		return;
	}
	uint32_t len = UD_FRAME_MAX - (t[2] & 0xffffff);
	uint32_t now = ut_now_ms();
	if ((t[2] >> 28) != 0 || len < ETHER_HDR_LEN || len > ETHER_MAX_LEN || (ut_last_rx_ms && now - ut_last_rx_ms > 1000)) {
		ut("RXd", t[2], now - (uint32_t)ut_last_rx_ms);
	}
	ut_last_rx_ms = now;
	ud_sync(UD_RX, UD_FRAME_MAX);
	if (ud.linked && len >= ETHER_HDR_LEN && len <= ETHER_MAX_LEN) {
		ud.rx_frames++;
		hakios_net_input_unit(UD_NET_UNIT, (const uint8_t *)(ud.dma + UD_RX), len);     /* copied there */
	}
	ud_rx_arm();
}

/* Start the next queued frame if the IN endpoint is idle. ud.lock held. */
static void
ud_tx_kick_locked(void)
{
	while (!ud.tx_busy && ud.txq_count > 0 && ud.configured) {
		unsigned slot = ud.txq_head;
		size_t len = ud.txlen[slot];

		ud.txq_head = (ud.txq_head + 1) % UD_TXQ;
		ud.txq_count--;
		if (len > ETHER_MAX_LEN - ETHER_CRC_LEN) {
			continue;
		}
		memcpy((void *)(ud.dma + UD_TX), ud_txq_frames[slot], len);
		/* A frame of a multiple of 512 bytes would need a zero-length
		 * packet to end it; one pad byte does the same (IP ignores it). */
		if ((len % 512) == 0) {
			((uint8_t *)(ud.dma + UD_TX))[len++] = 0;
		}
		ud.tx_busy = true;
		ud.tx_frames++;
		if (ud_start_trb(EP_TX, UD_TRB_TX, UD_TX, (uint32_t)len, TRBCTL_NORMAL) != 0) {
			ud.tx_busy = false;     /* refused: that frame is lost, the next ones are not */
			break;
		}
	}
}

/* hakios.net.c has a frame to send; the packet stays its own: copy it. */
static bool
ud_transmit(mbuf_t packet, size_t length)
{
	bool queued = false;

	if (ud.lock == NULL) {
		return false;
	}
	lck_mtx_lock(ud.lock);
	if (ud.txq_count < UD_TXQ && ud.configured && length <= UD_TXQ_FRAME) {
		unsigned slot = (ud.txq_head + ud.txq_count) % UD_TXQ;

		mbuf_copydata(packet, 0, length, ud_txq_frames[slot]);
		ud.txlen[slot] = (uint16_t)length;
		ud.txq_count++;
		queued = true;
		ud_tx_kick_locked();
	}
	lck_mtx_unlock(ud.lock);
	return queued;
}

/* the link behind en2 (hakios.net.c makes the interface; frames go through ud_transmit / hakios_net_input_unit) */
static bool ud_transmit(mbuf_t packet, size_t length);

static struct hakios_net_link ud_link = {
	.name = "USB CDC-ECM gadget (DWC3)",
	.address = { 0x02, 0x00, 0x5e, 0x77, 0xee, 0x02 },
	.unit = UD_NET_UNIT,
	.up = false,                    /* up when the host has set the configuration */
	.transmit = ud_transmit,
};

static void
ud_if_create(void)
{
	static lck_grp_t *grp;

	grp = lck_grp_alloc_init("hakios_usb", LCK_GRP_ATTR_NULL);
	ud.lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);
	ud.linked = hakios_net_attach(&ud_link);
	printf("usb: network interface en%u %s\n", UD_NET_UNIT, ud.linked ? "attached" : "NOT attached");
}

/*
 * A bus reset (the host's, or usbipd's when the phone was slow to answer) loses the data
 * endpoints' configuration in the controller. Until now only the address went back to 0: the
 * endpoints stayed "configured" here, so the next SET_CONFIGURATION set nothing up, a frame
 * being sent kept tx_busy, and the link was dead until a reboot. As Linux's
 * dwc3_gadget_reset_interrupt: end the data endpoints' transfers, forget the configuration and
 * the queue, restart a control transfer caught in the middle.
 */
static void
ud_bus_reset(void)
{
	for (unsigned ep = EP_RX; ep <= EP_INT; ep++) {
		ud_end_transfer(ep);
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) & ~((1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT)));
	if (ud.lock != NULL) {
		lck_mtx_lock(ud.lock);
	}
	ud.configured = false;
	if (ud.linked) {
		hakios_net_link_state_unit(UD_NET_UNIT, false);
	}
	ud.data_alt = 0;
	ud.tx_busy = false;
	ud.txq_head = 0;
	ud.txq_count = 0;
	if (ud.lock != NULL) {
		lck_mtx_unlock(ud.lock);
	}
	/*
	 * ep0: a SETUP already waiting stays; a control transfer caught in the middle is ended on
	 * both directions (Linux: dwc3_ep0_end_control_data) and ep0 armed again.
	 */
	if (ud.state != EP0_SETUP || !ud.xfer_active[0]) {
		ud_end_transfer(0);
		ud_end_transfer(1);
		ud_start_setup();
	}
}

static void
ud_event(uint32_t ev)
{
	if (ev & 1) {                   /* device event */
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {        /* USB reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			ut("RST", ud.resets, (uint32_t)ud.rx_frames);
			ud_bus_reset();
			if (ud.resets++ < 20) {
				printf("usb: bus reset\n");
			}
		} else if (type == 2) { /* connect done */
			uint32_t speed = dwc_rd(DSTS) & 7;
			/* ep0 max packet: 64 at high/full speed (modify action) */
			ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (0u << 25), 0);
			ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			    (1u << 8) | (1u << 10) | (1u << 25), 0);
			printf("usb: connected, speed %u\n", speed);
		} else if (type == 0) {
			printf("usb: disconnect\n");
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;
	unsigned status = (ev >> 12) & 0xf;

	if (ep >= EP_RX && ep <= EP_INT && (type != 1 || (status & 1))) {   /* not a completion, or a bus error */
		ut("EV", ev, (uint32_t)ud.rx_frames);
	}
	if (ep <= EP_INT && type == 1) {
		ud.xfer_active[ep] = false;
	}
	if (ep == EP_RX && type == 1) {
		ud_rx_done();
		return;
	}
	if (ep == EP_TX && type == 1) {
		lck_mtx_lock(ud.lock);
		ud.tx_busy = false;
		ud_tx_kick_locked();
		lck_mtx_unlock(ud.lock);
		return;
	}
	if (ep > 1) {
		return;
	}
	/*
	 * Control transfer state machine (Linux dwc3 ep0.c): SETUP completes ->
	 * request handled; the data stage (if any) is started right away; the
	 * status stage is started when the controller reports XferNotReady
	 * with status "control status"; its completion re-arms SETUP.
	 */
	if (type == 1) {                /* XferComplete */
		switch (ud.state) {
		case EP0_SETUP:
			ud_sync(UD_SETUP, 8);
			ud_setup();
			break;
		case EP0_DATA:
			ud.state = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
		return;
	}
	if (type == 3 && status == 2) { /* XferNotReady, control status phase */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.state = EP0_STATUS;
			ud_start(ep, UD_DATA, 0, ctl);
		}
	}
}

/*
 * Every 500 ms: a data transfer the controller has finished (HWO clear in its TRB) while no
 * event came for it, twice in a row with no frame moving. The link went dead that way under
 * load (rx and tx counters frozen for half a minute, no USB event, back at the next bus reset):
 * take the frame as if the event had come.
 */
static void
ud_watch(void)
{
	static bool rx_seen, tx_seen;
	volatile uint32_t *rt = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
	volatile uint32_t *tt = (volatile uint32_t *)(ud.dma + UD_TRB_TX);

	if (!ud.configured || ud.lock == NULL) {
		rx_seen = tx_seen = false;
		return;
	}
	ud_sync(UD_TRB_RX, 16);
	bool rx_idle = ud.xfer_active[EP_RX] && !(rt[3] & TRB_HWO) && ud.rx_frames == ud.watch_rx;
	if (rx_idle && rx_seen) {
		ut("RXL", ud.lost_rx, 0);
		if (ud.lost_rx++ < 20) {
			printf("usb: rx transfer done without an event (%u), taking it\n", ud.lost_rx);
		}
		ud.xfer_active[EP_RX] = false;
		ud_rx_done();
		rx_idle = false;
	}
	rx_seen = rx_idle;
	/*
	 * RX armed (HWO still set) and not a frame in 2 s: the link died that way under load too --
	 * the host's bulk OUT never completing, its cdc_ether "transmit queue timed out" every 5 s,
	 * until usbipd gave the device up, while the phone's own frames kept going out. End the
	 * transfer and arm it again; on a link that is merely idle that costs nothing.
	 */
	/* 0291: configured and no RX transfer at all (nothing armed): arm it */
	static unsigned rx_unarmed;
	if (!ud.xfer_active[EP_RX] && !(rt[3] & TRB_HWO)) {
		if (++rx_unarmed >= 2) {
			rx_unarmed = 0;
			ut("RXU", (uint32_t)ud.rx_frames, rt[3]);
			ud_rx_arm();
		}
	} else {
		rx_unarmed = 0;
	}
	static unsigned rx_silent;
	if (ud.xfer_active[EP_RX] && (rt[3] & TRB_HWO) && ud.rx_frames == ud.watch_rx) {
		if (++rx_silent >= 4) {
			rx_silent = 0;
			ud.rx_rearms++;
			ut("RRA", ud.rx_rearms, ((volatile uint32_t *)(ud.dma + UD_TRB_RX))[2]);
			ud_end_transfer(EP_RX);
			ud_rx_arm();
		}
	} else {
		rx_silent = 0;
	}
	ud.watch_rx = ud.rx_frames;

	lck_mtx_lock(ud.lock);
	ud_sync(UD_TRB_TX, 16);
	bool tx_idle = ud.tx_busy && !(tt[3] & TRB_HWO) && ud.tx_frames == ud.watch_tx;
	if (tx_idle && tx_seen) {
		if (ud.lost_tx++ < 20) {
			printf("usb: tx transfer done without an event (%u), going on\n", ud.lost_tx);
		}
		ud.xfer_active[EP_TX] = false;
		ud.tx_busy = false;
		ud_tx_kick_locked();
		tx_idle = false;
	}
	tx_seen = tx_idle;
	ud.watch_tx = ud.tx_frames;
	lck_mtx_unlock(ud.lock);
}

static int
sysctl_hakios_usb_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char buf[320];
	uint32_t rx3 = 0, tx3 = 0, dsts = 0;
	if (ud.dma != 0) {
		ud_sync(UD_TRB_RX, 16);
		ud_sync(UD_TRB_TX, 16);
		rx3 = ((volatile uint32_t *)(ud.dma + UD_TRB_RX))[3];
		tx3 = ((volatile uint32_t *)(ud.dma + UD_TRB_TX))[3];
		dsts = dwc_rd(DSTS);
	}
	snprintf(buf, sizeof(buf), "configured %d events %u resets %u; rx %llu frames (trb %08x, active %d) tx %llu "
	    "frames (trb %08x, busy %d, queue %u); without event: rx %u tx %u, late rx %u; rx rearms %u; halt clears %u; DSTS %08x",
	    ud.configured, ud.events, ud.resets, ud.rx_frames, rx3, ud.xfer_active[EP_RX], ud.tx_frames, tx3,
	    ud.tx_busy, ud.txq_count, ud.lost_rx, ud.lost_tx, ud.stale_rx, ud.rx_rearms, ud.halt_clears, dsts);
	return SYSCTL_OUT(req, buf, strlen(buf) + 1);
}
static int
sysctl_hakios_usb_trace SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	static char out[UT_N * 40];
	size_t n = 0;
	uint32_t start = ut_pos > UT_N ? ut_pos - UT_N : 0;
	for (uint32_t i = start; i < ut_pos && n + 48 < sizeof(out); i++) {
		struct ut_ent *e = &ut_ring[i % UT_N];
		n += (size_t)snprintf(out + n, sizeof(out) - n, "%u %s %08x %08x\n", e->ms, e->what, e->a, e->b);
	}
	out[n] = 0;
	return SYSCTL_OUT(req, out, n + 1);
}
SYSCTL_PROC(_kern, OID_AUTO, hakios_usb_trace, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_hakios_usb_trace, "A", "USB gadget trace: ms what a b");

SYSCTL_PROC(_kern, OID_AUTO, hakios_usb_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_hakios_usb_stats, "A", "USB Ethernet gadget state");

static void
ud_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint32_t ticks = 0;
	for (;;) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		if (count) {
			ud_sync(UD_EVT, UD_EVT_SIZE);
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);
				ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
				if (ud.events++ < 200) {
					printf("usb: event 0x%08x\n", ev);
				}
				ud_event(ev);
			}
			dwc_wr(GEVNTCOUNT, count);
		}
		if ((ticks % 500) == 0) {
			ud_watch();
		}
		if ((++ticks % 30000) == 0) {
			printf("usb: en2 rx %llu tx %llu frames\n", ud.rx_frames, ud.tx_frames);
		}
		ud_sleep_ms(1);
	}
}

static void
hakios_usb_gadget_start(vm_offset_t dwc)
{
	thread_t thread;

	ud.dwc = dwc;
	if (kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("usb: no DMA memory\n");
		return;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)ud.dma);
	if (!pn) {
		return;
	}
	ud.dma_pa = ptoa_64(pn);
	ud_if_create();

	/* Device controller soft reset (PHY and global setup stay as ABL left them). */
	dwc_wr(DCTL, DCTL_CSFTRST);
	int i;
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++) {
		delay(1000);
	}
	printf("usb: soft reset %s after %d ms, GCTL 0x%08x DCFG 0x%08x\n",
	    (dwc_rd(DCTL) & DCTL_CSFTRST) ? "STUCK" : "done", i, dwc_rd(GCTL), dwc_rd(DCFG));
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		return;
	}

	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;

	/* High speed, address 0 */
	uint32_t dcfg = dwc_rd(DCFG) & ~(7u | (0x7fu << 3));
	dwc_wr(DCFG, dcfg | 0);
	/* Events: disconnect, USB reset, connect done */
	dwc_wr(DEVTEN, 0x7);

	/* Endpoint 0 OUT/IN: start config, control type, max packet 512 until connect done. */
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("usb: endpoint 0 setup failed\n");
		return;
	}
	dwc_wr(DALEPENA, 0x3);
	ud_start_setup();

	vm_offset_t qs = ml_io_map(QSCRATCH_PA, 0x100);
	if (qs != 0) {
		uint32_t hs = usb_rd(qs, QSCRATCH_HS_PHY_CTRL), ss = usb_rd(qs, QSCRATCH_SS_PHY_CTRL);
		printf("usb: QSCRATCH HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x GENERAL_CFG 0x%08x\n",
		    hs, ss, usb_rd(qs, 0x08));
		*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL;
		*(volatile uint32_t *)(qs + QSCRATCH_SS_PHY_CTRL) = ss | LANE0_PWR_PRESENT;
		__asm__ volatile ("dsb sy" ::: "memory");
		printf("usb: VBUS override on: HS_PHY_CTRL 0x%08x SS_PHY_CTRL 0x%08x\n",
		    usb_rd(qs, QSCRATCH_HS_PHY_CTRL), usb_rd(qs, QSCRATCH_SS_PHY_CTRL));
	}

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++) {
		delay(1000);
	}
	printf("usb: running (DSTS 0x%08x after %d ms), DMA at 0x%llx\n", dwc_rd(DSTS), i, ud.dma_pa);
	if (kernel_thread_start(ud_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
