/*
 * Polled SPMI (PMIC arbiter v5) reads for the charging screen. See pmic.h.
 * Register provenance: xnu/patches/0062 (arbiter + observer), 0092/0095 (PMI8998 FG gen3 + SMB2,
 * Linux qpnp-fg-gen3.c / smb-reg.h), 0126/0137 (WLED sink + write channel, qcom-wled.c WLED4),
 * 0060/0082 (PON RT status: bit0 KPDPWR, bit1 RESIN).
 *
 * No static pointer tables (build.sh rejects ABS64); MMU off, so all MMIO is Device memory.
 */
#include "board.h"
#include "pmic.h"

#if defined(BOARD_SAPPHIRE)
/*
 * sapphire (SM6225 bengal/khaje): the PMIC arbiter v5 is NOT where sweet/beryllium have it. Addresses
 * from the working iOS 16 port (xnu 0207 sweet_spmi.c, sweet_keys.c; a read at the old sweet addresses
 * never returns) and the bengal device tree: core 0x01c40000, cnfg 0x01c0a000, obsrvr 0x03e00000.
 * Read-only here: the charger registers (SMB5) and the gauge (QG) of the PM7250B (SID 2, 0206/0208) and
 * the PM6125 PON (SID 0). No write is reachable (AMOLED panel: no WLED; charging runs in hardware).
 */
#define SP_CORE	0x01c40000UL
#define SP_CFG	0x01c0a000UL
#define SP_OBS	0x03e00000UL
#define SP_CHNL	0x01e00000UL	/* bengal DT "chnls"; unused, writes are refused on sapphire */
#define SP_NO_APID	0xffffU
#define SP_CACHE	12

#define SID_PM8998	0	/* PM6125: PON at 0x800 (topaz/sapphire sm6125 spmi_bus, patch 0087) */
#define SID_PMI8998	2	/* PM7250B: SMB5 charger 0x10xx/0x11xx/0x13xx, QG gauge 0x48xx */
#define SID_WLED	3	/* no WLED on the AMOLED phone: never addressed */

#define CHGR_STATUS_1	0x1006	/* BATTERY_CHARGER_STATUS_1: [2:0] state (same layout as PM6150) */
#define CHGR_ERROR	0x1007
#define USBIN_APSD	0x1308
#define USBIN_RT	0x1310	/* bit4 plug-in */
#define QG_TYPE		0x4804	/* must read 0x0d */
#define QG_SUBTYPE	0x4805	/* 3 or 4: current scale 152588 / 305176 nA per bit */
#define QG_PRESENT	0x4808
/* voltage, then the matching current register; 0208: LAST_ADC (0xc0) stays 0 until Linux's QG ran */
static const uint16_t qg_v[] = { 0x48c0, 0x4880, 0x4890, 0x4870 };
static const uint16_t qg_i[] = { 0x48c2, 0x4882, 0x48a0, 0x4872 };

#define PON_REASON1	0x0808
#define PON_RT		0x0810
#else

#define SP_CORE	0x0c440000UL	/* arbiter core: +0 version, +4 APID count, +0x900 + 4*apid ppid map */
#define SP_CFG	0x0c40a000UL	/* +0x700 + 4*apid: owner EE */
#define SP_OBS	0x0e600000UL	/* observer, 0x80 per APID: CMD +0, STATUS +8, RDATA +0x18 */
#define SP_CHNL	0x0c600000UL	/* read/write channels, 0x10000 per APID: CMD +0, STATUS +8, WDATA +0x10 */
#define SP_NO_APID	0xffffU
#define SP_CACHE	12

#define SID_PM8998	0
#define SID_PMI8998	2
#define SID_WLED	3	/* PMI8998 WLED lives on the second PMI8998 SID */

/* PMI8998 SMB2 charger / FG gen3 registers (SID 2) */
#define CHGR_STATUS_1	0x1006	/* BATTERY_CHARGER_STATUS_1: [2:0] state */
#define USBIN_APSD	0x1308	/* APSD_RESULT_STATUS: bit0 SDP, 1 OCP, 2 CDP, 3 DCP, 4 float, 5 QC2, 6 QC3 (smb-reg.h; sweet_charger.c) */
#define USBIN_RT	0x1310	/* USBIN INT_RT_STS: bit4 plug-in */
#define FG_MSOC		0x4009	/* BATT_SOC monotonic SOC, 0..255 */
#define FG_VOLT		0x41a0	/* BATT_INFO voltage, 16 bit */
#define FG_CURR		0x41a2	/* BATT_INFO current, 16 bit signed, + = discharging */

/* PM8998 PON (SID 0, base 0x800) */
#define PON_REASON1	0x0808
#define PON_RT		0x0810

/* PMI8998 WLED (SID 3): control 0xd800, sink 0xd900 */
#define WLED_MOD_EN	0xd846
#define WLED_SYNC	0xd947
#define WLED_BRIGHT(n)	(0xd957 + 0x10 * (n))
#define WLED_SYNC_MASK	0x0f
#endif /* BOARD_SAPPHIRE */

static int sp_ready;
static unsigned sp_count;
static uint8_t sp_poisoned[512];	/* a timed-out command may still complete: never reuse that lane */
static struct { uint16_t ppid, apid; } sp_cache[SP_CACHE];
static unsigned sp_ncache;

static inline uint32_t rd(uintptr_t a)
{
	uint32_t v = *(volatile uint32_t *)a;
	__asm__ volatile("dmb sy" ::: "memory");
	return v;
}

static inline void wr(uintptr_t a, uint32_t v)
{
	__asm__ volatile("dmb sy" ::: "memory");
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(unsigned us)
{
	uint64_t f, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	uint64_t end = t + f / 1000000 * us;
	do
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	while (t < end);
}

/* The v5 arbiter can map one PPID more than once: prefer the apps-owned (EE 0) entry. */
static uint16_t find_apid(unsigned ppid)
{
	uint16_t found = SP_NO_APID;

	for (unsigned i = 0; i < sp_ncache; i++)
		if (sp_cache[i].ppid == ppid)
			return sp_cache[i].apid;
	for (unsigned i = 0; i < sp_count; i++) {
		uint32_t map = rd(SP_CORE + 0x900 + 4 * i);
		if (!map || ((map >> 8) & 0xfff) != ppid)
			continue;
		unsigned owner = rd(SP_CFG + 0x700 + 4 * i) & 7;
		if (found == SP_NO_APID || owner == 0)
			found = (uint16_t)i;
	}
	if (found != SP_NO_APID && sp_ncache < SP_CACHE) {
		sp_cache[sp_ncache].ppid = (uint16_t)ppid;
		sp_cache[sp_ncache].apid = found;
		sp_ncache++;
	}
	return found;
}

int pmic_init(void)
{
	uint32_t ver = rd(SP_CORE);

	sp_count = rd(SP_CORE + 4) & 0x7ff;
	if (ver < 0x50000000u || ver >= 0x70000000u || !sp_count || sp_count > 512) {
		sp_ready = 0;
		return -1;
	}
	sp_ready = 1;
	return 0;
}

/* Read allow-list: only the blocks this screen needs (same as the kernel's whitelist plus PON). */
static int read_allowed(unsigned sid, unsigned addr)
{
	unsigned p = addr >> 8;

#if defined(BOARD_SAPPHIRE)
	if (sid == SID_PMI8998)	/* SMB5 CHGR 0x10, DC path 0x11, USBIN 0x13, QG 0x48 */
		return p == 0x10 || p == 0x11 || p == 0x13 || p == 0x48;
#else
	if (sid == SID_PMI8998)
		return p == 0x10 || p == 0x13 || p == 0x16 || p == 0x40 || p == 0x41;
#endif
	if (sid == SID_PM8998)
		return p == 0x08;
	if (sid == SID_WLED)
		return p == 0xd8 || p == 0xd9;
	return 0;
}

static int spmi_read(unsigned sid, unsigned addr, uint8_t *out)
{
	if (!sp_ready || addr > 0xffff || !read_allowed(sid, addr))
		return -1;
	uint16_t apid = find_apid((sid << 8) | (addr >> 8));
	if (apid == SP_NO_APID || sp_poisoned[apid])
		return -1;
	uintptr_t off = 0x80UL * apid;
	wr(SP_OBS + off, (1u << 27) | ((addr & 0xff) << 4));	/* EXT_READL, 1 byte */
	for (unsigned i = 0; i < 2000; i++) {
		uint32_t st = rd(SP_OBS + off + 0x08);
		if (st & 1u) {
			if (st & 0x0e)
				return -1;
			*out = (uint8_t)rd(SP_OBS + off + 0x18);
			return 0;
		}
		udelay(1);
	}
	sp_poisoned[apid] = 1;
	return -1;
}

/* Dual-read from shadow registers (addr+2, addr+3) for torn sample detection.
 * The PMIC maintains shadow copies of ADC result registers at offset +2. */
static int read_word(unsigned sid, unsigned addr, uint16_t *v)
{
	uint8_t lo, hi, lo_shadow, hi_shadow;

	/* Read primary registers */
	if (spmi_read(sid, addr, &lo) || spmi_read(sid, addr + 1, &hi))
		return -1;
	/* Read shadow registers */
	if (spmi_read(sid, addr + 2, &lo_shadow) || spmi_read(sid, addr + 3, &hi_shadow))
		return -1;

	uint16_t primary = (uint16_t)(lo | hi << 8);
	uint16_t shadow = (uint16_t)(lo_shadow | hi_shadow << 8);

	if (primary != shadow)
		return -1;	/* torn sample */

	*v = primary;
	return 0;
}

#if defined(BOARD_SAPPHIRE)
/* The generic single-cell curve of the kernel's SweetBattery::estimatePercent (xnu 0091). */
static unsigned sapphire_soc(unsigned mv)
{
	static const unsigned volts[] = { 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200, 4350 };
	static const unsigned soc[] = { 1, 5, 10, 20, 40, 60, 75, 85, 93, 100 };

	if (mv <= volts[0])
		return soc[0];
	for (unsigned i = 1; i < 10; i++)
		if (mv < volts[i])
			return soc[i - 1] + (mv - volts[i - 1]) * (soc[i] - soc[i - 1]) / (volts[i] - volts[i - 1]);
	return 100;
}
#endif

void pmic_read_batt(struct pmic_batt *b)
{
#if !defined(BOARD_SAPPHIRE)
	uint8_t msoc = 0, msoc2 = 1;
#endif
	uint8_t usb = 0, chg = 0, apsd = 0;
	uint16_t rv = 0, rc = 0;
	int ok_usb, ok_chg;

	b->valid = b->ma_valid = b->usb_present = b->ma = 0;
	b->soc = b->mv = b->chg_state = b->apsd = b->msoc_raw = 0;
	b->chg = PMIC_CHG_NONE;
	if (!sp_ready)
		return;

	ok_usb = !spmi_read(SID_PMI8998, USBIN_RT, &usb);
	ok_chg = !spmi_read(SID_PMI8998, CHGR_STATUS_1, &chg);
	if (!spmi_read(SID_PMI8998, USBIN_APSD, &apsd))
		b->apsd = apsd;
	b->usb_present = ok_usb && (usb & 0x10);
	b->chg_state = chg & 7;
	if (ok_usb && ok_chg && b->usb_present) {
		if (b->chg_state >= 1 && b->chg_state <= 4)
			b->chg = PMIC_CHG_CHARGING;
		else if (b->chg_state == 5)
			b->chg = PMIC_CHG_FULL;
	}

#if defined(BOARD_SAPPHIRE)
	/*
	 * PM7250B QG (xnu 0206/0208): battery present + type 0x0d, then the first QG voltage register that holds a
	 * plausible value (194.637 uV/bit), current from the matching register (sign: Linux negative = charging).
	 */
	uint8_t qtype = 0, qpres = 0, qsub = 0;
	unsigned which, mv = 0;

	if (spmi_read(SID_PMI8998, QG_TYPE, &qtype) || qtype != 0x0d || spmi_read(SID_PMI8998, QG_PRESENT, &qpres) ||
	    !(qpres & 1u))
		return;
	for (which = 0; which < 4; which++) {
		if (!read_word(SID_PMI8998, qg_v[which], &rv) && rv != 0 && rv != 0x8000) {
			mv = (unsigned)(((uint64_t)rv * 194637u) / 1000000u);
			if (mv >= 2800 && mv <= 4600)
				break;
		}
		mv = 0;
	}
	if (!mv)
		return;
	b->mv = mv;
	b->soc = sapphire_soc(mv);
	if (b->chg == PMIC_CHG_FULL)
		b->soc = 100;
	if (!spmi_read(SID_PMI8998, QG_SUBTYPE, &qsub) && (qsub == 3 || qsub == 4) &&
	    !read_word(SID_PMI8998, qg_i[which], &rc) && rc != 0x8000) {
		int raw = (rc & 0x8000) ? (int)rc - 65536 : (int)rc;
		b->ma = (int)(-((int64_t)raw * (qsub == 3 ? 152588 : 305176)) / 1000000);
		b->ma_valid = 1;
	}
	b->valid = 1;
}
#else
	/* FG: two equal reads of the monotonic SOC (fg_get_msoc_raw) */
	int ok = 0;
	for (unsigned i = 0; i < 5 && !ok; i++)
		ok = !spmi_read(SID_PMI8998, FG_MSOC, &msoc) && !spmi_read(SID_PMI8998, FG_MSOC, &msoc2) &&
		     msoc == msoc2;
	if (!ok || read_word(SID_PMI8998, FG_VOLT, &rv))
		return;
	unsigned mv = (unsigned)(((uint64_t)rv * 122070u) / 1000000u);
	if (mv < 2800 || mv > 4600)
		return;
	b->mv = mv;
	b->msoc_raw = msoc;
	b->soc = msoc == 255 ? 100 : msoc == 0 ? 0 : ((unsigned)(msoc - 1) * 98u + 252u) / 253u + 1u;
	if (b->chg == PMIC_CHG_FULL)
		b->soc = 100;
	if (!read_word(SID_PMI8998, FG_CURR, &rc)) {
		int raw = (rc & 0x8000) ? (int)rc - 65536 : (int)rc;
		/* FG: + = discharging, 488.281 uA/bit; here + = charging */
		b->ma = (int)(-((int64_t)raw * 488281) / 1000000);
		b->ma_valid = 1;
	}
	b->valid = 1;
}
#endif

unsigned pmic_keys(void)
{
	uint8_t v;

	if (spmi_read(SID_PM8998, PON_RT, &v))
		return PMIC_KEY_ERR;
	return v & (PMIC_KEY_POWER | PMIC_KEY_VOLDN);
}

int pmic_pon_reason(void)
{
	uint8_t v;

	return spmi_read(SID_PM8998, PON_REASON1, &v) ? -1 : v;
}

/* ---- WLED backlight (the one write path) ---- */

static int write_allowed(unsigned sid, unsigned addr, unsigned len)
{
#if defined(BOARD_SAPPHIRE)
	(void)sid; (void)addr; (void)len;
	return 0;	/* sapphire: read-only (charging runs in hardware, the panel is AMOLED) */
#endif
	if (sid == SID_PMI8998) {
		/*
		 * SMB2 charger, single bytes: the seven registers xnu's sp_write_allowed lets sweetcharger=2
		 * through (patches-11215/0200), nothing else (no HVDCP/QC, type-C, JEITA, termination, AICL).
		 */
		if (len != 1)
			return 0;
		switch (addr) {
		case 0x1042:	/* CHARGING_ENABLE_CMD */
		case 0x1061:	/* FAST_CHARGE_CURRENT_CFG */
		case 0x1070:	/* FLOAT_VOLTAGE_CFG */
		case 0x1340:	/* USBIN_CMD_IL (suspend) */
		case 0x1365:	/* USBIN_LOAD_CFG */
		case 0x1366:	/* USBIN_ICL_OPTIONS */
		case 0x1370:	/* USBIN_CURRENT_LIMIT_CFG */
			return 1;
		default:
			return 0;
		}
	}
	if (sid != SID_WLED)
		return 0;
	switch (addr) {
	case 0xd947:	/* sink SYNC */
		return len == 1;
	case 0xd957:	/* string 0 / 1 brightness, 16 bit */
	case 0xd967:
		return len >= 1 && len <= 2;
	default:
		return 0;
	}
}

/* EXT_WRITEL of 1..2 bytes through the APID's write channel, only if EE 0 owns the APID. */
static int spmi_write_sid(unsigned sid, unsigned addr, uint32_t data, unsigned len)
{
	if (!sp_ready || !write_allowed(sid, addr, len))
		return -1;
	uint16_t apid = find_apid((sid << 8) | (addr >> 8));
	if (apid == SP_NO_APID || sp_poisoned[apid] || (rd(SP_CFG + 0x700 + 4 * apid) & 7) != 0)
		return -1;
	uintptr_t ch = SP_CHNL + 0x10000UL * apid;
	wr(ch + 0x10, data);
	wr(ch + 0x00, ((addr & 0xff) << 4) | ((len - 1) & 7));	/* opcode 0 = EXT_WRITEL */
	for (unsigned i = 0; i < 2000; i++) {
		uint32_t st = rd(ch + 0x08);
		if (st & 1u)
			return (st & 0x0e) ? -1 : 0;
		udelay(1);
	}
	sp_poisoned[apid] = 1;
	return -1;
}

static int spmi_write(unsigned addr, uint32_t data, unsigned len)
{
	return spmi_write_sid(SID_WLED, addr, data, len);
}

int pmic_chg_read(unsigned addr, uint8_t *v)
{
	return spmi_read(SID_PMI8998, addr, v);
}

/* One whitelisted SMB2 register, written and read back; 0 only when the readback matches. */
int pmic_chg_write(unsigned addr, uint8_t v)
{
	uint8_t got;

	if (spmi_write_sid(SID_PMI8998, addr, v, 1))
		return -1;
	return !spmi_read(SID_PMI8998, addr, &got) && got == v ? 0 : -1;
}

#if defined(BOARD_SAPPHIRE)
/* AMOLED panel: there is no WLED to drive. */
int pmic_wled_get(uint16_t v[2]) { (void)v; return -1; }
int pmic_wled_set(const uint16_t v[2]) { (void)v; return -1; }
int pmic_wled_enabled(void) { return 0; }
#else
int pmic_wled_get(uint16_t v[2])
{
	for (unsigned n = 0; n < 2; n++) {
		uint8_t lo, hi;
		if (spmi_read(SID_WLED, WLED_BRIGHT(n), &lo) || spmi_read(SID_WLED, WLED_BRIGHT(n) + 1, &hi))
			return -1;
		v[n] = (uint16_t)(lo | hi << 8);
	}
	return 0;
}

int pmic_wled_enabled(void)
{
	uint8_t m;

	return !spmi_read(SID_WLED, WLED_MOD_EN, &m) && (m & 0x80);
}

static int wled_put(const uint16_t v[2])
{
	for (unsigned n = 0; n < 2; n++)
		if (spmi_write(WLED_BRIGHT(n), v[n] & 0xfff, 2))
			return -1;
	/* latch: clear, then set the string mask (qcom-wled.c wled3_sync_toggle) */
	if (spmi_write(WLED_SYNC, 0, 1) || spmi_write(WLED_SYNC, WLED_SYNC_MASK, 1))
		return -1;
	return 0;
}

int pmic_wled_set(const uint16_t v[2])
{
	uint16_t old[2], got[2];

	if (pmic_wled_get(old))
		return -1;
	if (wled_put(v) == 0) {
		udelay(300);	/* let the sink latch before reading back */
		if (pmic_wled_get(got) == 0 && (got[0] & 0xfff) == (v[0] & 0xfff) &&
		    (got[1] & 0xfff) == (v[1] & 0xfff))
			return 0;
	}
	(void)wled_put(old);	/* best effort back to what was there */
	return -1;
}
#endif
