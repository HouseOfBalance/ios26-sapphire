/*
 * Battery monitor for Xiaomi redwood (POCO X5 Pro).
 *
 * Redwood selects PM7325B and qcom,battery-charger over PMIC GLINK/ADSP, not
 * the local PM7250B/QG layout.  The resident UEFI contains the matching
 * Qualcomm charger drivers.  xnuloader owns that firmware context on core 0,
 * polls it, and publishes a read-only cache; XNU only consumes that cache.
 * Charger limits and JEITA protection therefore remain firmware-owned.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)
#define LOADER_PAGE_SIZE                0x4000UL

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent, voltage_mv, current_ma, temperature_c;
	uint32_t charger_action, polls, failures, reserved;
};

static volatile struct loader_battery *shared_battery;
static int battery_soc_percent = -1, battery_voltage_mv = -1, battery_current_ma;
static int battery_temperature_c = -273, battery_present, battery_charger_present;
static int battery_charging, battery_soc_estimated, battery_charger_action = -1;
static uint64_t battery_updates, battery_failures;

/* for others in the kernel (the flashlight's thermal guard): -273 while it is not known */
int
redwood_battery_temperature(void)
{
	return battery_temperature_c;
}

SYSCTL_NODE(_kern, OID_AUTO, redwood, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "redwood phone bits");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_percent, 0, "battery state of charge, 0..100 percent, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_voltage_mv, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_voltage_mv, 0, "battery voltage in millivolts, or -1 if unknown");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_current_ma, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_current_ma, 0, "battery current reported by Qualcomm firmware, in milliamps");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_temperature_c, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_temperature_c, 0, "battery temperature reported by Qualcomm firmware, in Celsius");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_present, 0, "1 if Qualcomm firmware reports a battery");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charger_present, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_present, 0, "1 if Qualcomm firmware reports an attached charger");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charging, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charging, 0, "1 while an attached charger is charging a non-full battery");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_soc_estimated, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_soc_estimated, 0, "1 only when SOC had to be estimated from voltage");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_charger_action, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_charger_action, 0, "last Qualcomm charger action, for diagnostics");

static bool
battery_snapshot(struct loader_battery *out)
{
	uint32_t before, after;

	for (unsigned retry = 0; retry < 4; retry++) {
		before = shared_battery->sequence;
		if (before & 1) continue;
		__asm__ volatile ("dmb ishld" ::: "memory");
		memcpy(out, (const void *)(uintptr_t)shared_battery, sizeof(*out));
		__asm__ volatile ("dmb ishld" ::: "memory");
		after = shared_battery->sequence;
		if (before == after && !(after & 1)) return true;
	}
	return false;
}

static void
battery_poll(void)
{
	struct loader_battery sample;

	if (!battery_snapshot(&sample) || sample.magic != LOADER_BATTERY_MAGIC ||
	    sample.version != LOADER_BATTERY_VERSION || sample.size < sizeof(sample) ||
	    !(sample.flags & LOADER_BATTERY_VALID) || sample.soc_percent < 0 || sample.soc_percent > 100) {
		battery_failures++;
		return;
	}
	battery_soc_percent = sample.soc_percent;
	battery_voltage_mv = (sample.flags & LOADER_BATTERY_VOLTAGE_VALID) ? sample.voltage_mv : -1;
	battery_current_ma = sample.current_ma;
	battery_temperature_c = sample.temperature_c;
	battery_present = (sample.flags & LOADER_BATTERY_PRESENT) != 0;
	battery_charger_present = (sample.flags & LOADER_BATTERY_CHARGER_PRESENT) != 0;
	battery_charging = (sample.flags & LOADER_BATTERY_CHARGING) != 0;
	battery_soc_estimated = (sample.flags & LOADER_BATTERY_SOC_ESTIMATED) != 0;
	battery_charger_action = (int)sample.charger_action;
	battery_updates++;
}

/*
 * What the audio DSP's battery manager says (bsd/dev/redwood_adsp.c, with adsp=1 unless battmgr=0):
 * the values that Android shows, from where Android takes them. They are kept here for the sysctls
 * and passed on to the power source that iOS reads (iokit/Kernel/RedwoodBattery.cpp).
 */
static int battery_full_uah, battery_design_uah, battery_cycles, battery_source_dsp;

SYSCTL_INT(_kern_redwood, OID_AUTO, battery_full_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_full_uah, 0, "the battery's full charge as the fuel gauge has learnt it, in uAh (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_design_uah, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_design_uah, 0, "the battery's charge as designed, in uAh (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_cycles, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_cycles, 0, "the battery's charge cycles (0 if unknown)");
SYSCTL_INT(_kern_redwood, OID_AUTO, battery_from_dsp, CTLFLAG_RD | CTLFLAG_LOCKED,
    &battery_source_dsp, 0, "1 when the values come from the audio DSP's battery manager");

extern void RedwoodBatteryReport(const void *state, unsigned size);     /* iokit/Kernel/RedwoodBattery.cpp */

void
redwood_battery_report(const struct redwood_battery_state *b)
{
	int percent = (b->percent_hundredths + 50) / 100;

	battery_soc_percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	battery_voltage_mv = b->voltage_uv / 1000;
	battery_current_ma = b->current_ua / 1000;
	battery_temperature_c = (b->temperature_hundredths + 50) / 100;
	battery_present = b->present != 0;
	battery_charger_present = b->external != 0;
	battery_charging = b->status == 1;      /* POWER_SUPPLY_STATUS_CHARGING */
	battery_soc_estimated = 0;
	battery_charger_action = b->status;
	if (b->full_uah > 0) {
		battery_full_uah = b->full_uah;
	}
	if (b->design_uah > 0) {
		battery_design_uah = b->design_uah;
	}
	if (b->cycles > 0) {
		battery_cycles = b->cycles;
	}
	battery_source_dsp = 1;
	battery_updates++;
	RedwoodBatteryReport(b, sizeof(*b));
}

/*
 * boot-arg battery=2: the Redmi Note 13 4G (sapphire). Its charger and fuel gauge are the PM7250B's (SPMI slave 2: SMB5 at
 * 0x1000-0x13ff, QG at 0x4800), not redwood's PMIC GLINK ones, and there is no xnuloader cache: read them through the
 * PMIC arbiter's observer, as the iOS 16 port does (xnu/patches 0206-0208). The arbiter is version 5 at the SM6225's
 * addresses, the same one the buttons read (redwood.buttons.c); one transfer at a time (redwood_spmi_lock()).
 * The percentage is an estimate from the voltage (the QG is not set going by anyone here), so soc_estimated is 1.
 */
#define SAPPHIRE_ARB_CORE       0x01c40000UL
#define SAPPHIRE_ARB_CNFG       0x01c0a000UL
#define SAPPHIRE_ARB_OBSERVER   0x03e00000UL
#define SAPPHIRE_PMIC_SID       2               /* boot-arg battery_sid=0: PM6125 */
#define SP_NO_CHANNEL           0xffffu

static vm_offset_t sp_core, sp_obs;
static uint32_t sp_channels;
static uint32_t sp_sid = SAPPHIRE_PMIC_SID;

static uint16_t
sp_find_channel(uint16_t ppid)
{
	for (uint32_t n = 0; n < sp_channels; n++) {
		uint32_t map = *(volatile uint32_t *)(sp_core + 0x900 + 4 * n);

		if (map != 0 && ((map >> 8) & 0xfff) == ppid) {
			return (uint16_t)n;
		}
	}
	return SP_NO_CHANNEL;
}

/* one register; 0 or what went wrong. Only the battery's blocks (SMB5 0x10-0x13, QG 0x48). */
static int
sp_read(uint32_t sid, uint32_t addr, uint8_t *out)
{
	static uint16_t cache_ppid[8], cache_channel[8];
	static unsigned cached;
	static bool poisoned[512];
	uint32_t peripheral = addr >> 8;
	uint16_t ppid = (uint16_t)(sid << 8 | peripheral), channel = SP_NO_CHANNEL;

	if (sp_obs == 0 || sid > 15 || addr > 0xffffu || (peripheral < 0x10 || (peripheral > 0x13 && peripheral != 0x48))) {
		return -1;
	}
	for (unsigned i = 0; i < cached; i++) {
		if (cache_ppid[i] == ppid) {
			channel = cache_channel[i];
		}
	}
	if (channel == SP_NO_CHANNEL) {
		channel = sp_find_channel(ppid);
		if (channel == SP_NO_CHANNEL) {
			return -2;
		}
		if (cached < 8) {
			cache_ppid[cached] = ppid;
			cache_channel[cached++] = channel;
		}
	}
	if (poisoned[channel & 511]) {
		return -2;
	}
	vm_offset_t reg = sp_obs + 0x80UL * channel;     /* observer of system 0 */
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status;

	nanoseconds_to_absolutetime(2000 * (uint64_t)NSEC_PER_USEC, &interval);
	redwood_spmi_lock();
	*(volatile uint32_t *)(reg + 0x00) = (1u << 27) | (addr & 0xffu) << 4;    /* extended register read long, 1 byte */
	do {
		status = *(volatile uint32_t *)(reg + 0x08);
		now = mach_absolute_time();
	} while (!(status & 1u) && now - start < interval);
	*out = (uint8_t)*(volatile uint32_t *)(reg + 0x18);
	redwood_spmi_unlock();
	if (!(status & 1u)) {
		poisoned[channel & 511] = true;         /* a late completion must not be mistaken for the next answer */
		return -4;
	}
	return (status & 0x0eu) ? -3 : 0;
}

static bool
sp_word(uint32_t sid, uint32_t addr, uint16_t *value)
{
	uint16_t previous = 0;

	/* two identical reads in a row, to reject a torn ADC sample (the QG's HOLD bit is not written) */
	for (unsigned attempt = 0; attempt < 4; attempt++) {
		uint8_t lo, hi;

		if (sp_read(sid, addr, &lo) || sp_read(sid, addr + 1, &hi)) {
			return false;
		}
		uint16_t next = (uint16_t)(lo | ((unsigned)hi << 8));

		if (attempt != 0 && next == previous) {
			*value = next;
			return true;
		}
		previous = next;
	}
	return false;
}

/* a generic single-cell curve: an estimate, with load sag, temperature and ageing in its error */
static unsigned
sp_percent(unsigned mv)
{
	static const unsigned volts[] = { 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200, 4350 };
	static const unsigned soc[] = { 1, 5, 10, 20, 40, 60, 75, 85, 93, 100 };

	if (mv <= volts[0]) {
		return soc[0];
	}
	for (unsigned i = 1; i < sizeof(volts) / sizeof(volts[0]); i++) {
		if (mv < volts[i]) {
			return soc[i - 1] + (mv - volts[i - 1]) * (soc[i] - soc[i - 1]) / (volts[i] - volts[i - 1]);
		}
	}
	return 100;
}

static void
sapphire_battery_poll(void)
{
	const uint32_t sid = sp_sid;
	uint8_t type = 0, present = 0, usb = 0, path = 0, charger = 0, error = 0, temperature = 0, msoc = 0, subtype = 0;
	uint16_t rawVoltage = 0, rawCurrent = 0;
	static const uint16_t vregs[] = { 0x48c0, 0x4880, 0x4890, 0x4870 }, iregs[] = { 0x48c2, 0x4882, 0x48a0, 0x4872 };
	unsigned which = 0, mv = 0;
	static uint32_t bad;

	/* the QG and the charger: type 0x0d is the QG; SMB5 USB input status, input path, charger state, error, temperature */
	bool regs = !sp_read(sid, 0x4804, &type) && type == 0x0d && !sp_read(sid, 0x4808, &present) &&
	    !sp_read(sid, 0x1310, &usb) && !sp_read(sid, 0x110b, &path) && !sp_read(sid, 0x1006, &charger) &&
	    !sp_read(sid, 0x1007, &error) && !sp_read(sid, 0x100d, &temperature);

	if (!regs) {
		battery_failures++;
		if (bad++ % 30 == 0) {
			printf("battery: PM7250B (sid %u) does not answer (QG type %#x)\n", sid, type);
		}
		return;
	}
	/* LAST_ADC_V stays 0 until a Linux QG driver sets the gauge going: then the S2 average, the newest FIFO sample, the OCV */
	for (; which < 4; which++) {
		if (sp_word(sid, vregs[which], &rawVoltage) && rawVoltage != 0 && rawVoltage != 0x8000) {
			mv = (unsigned)(((uint64_t)rawVoltage * 194637u) / 1000000u);
			if (mv >= 2800 && mv <= 4600) {
				break;
			}
		}
		mv = 0;
	}
	bool haveV = mv != 0;

	(void)sp_read(sid, 0x48bf, &msoc);              /* QG_SOC_MONOTONIC: what the boot's charging code left */
	bool external = (usb & 0x10u) != 0;
	bool capable = external && (path & 0x10u) && (path & 1u) && !(path & 0x40u);
	unsigned state = charger & 7u;
	bool charging = capable && state >= 1 && state <= 4 && !(error & 2u);
	bool full = external && state == 5 && !(error & 2u);
	int32_t current_ua = 0;

	if (haveV && !sp_read(sid, 0x4805, &subtype) && (subtype == 3 || subtype == 4) &&
	    sp_word(sid, iregs[which < 4 ? which : 0], &rawCurrent) && rawCurrent != 0x8000) {
		int raw = (rawCurrent & 0x8000u) ? (int)rawCurrent - 65536 : (int)rawCurrent;

		/* negative raw = charging; the queue wants more than 0 while the battery takes charge */
		current_ua = (int32_t)(-((int64_t)raw * (subtype == 3 ? 152588 : 305176)) / 1000);
	}
	unsigned percent = haveV ? sp_percent(mv) : (msoc >= 1 && msoc <= 100) ? msoc : 50;
	struct redwood_battery_state b = { .size = sizeof(b) };

	b.percent_hundredths = (int32_t)((full ? 100u : percent) * 100u);
	b.voltage_uv = haveV ? (int32_t)(mv * 1000u) : 3900000;
	b.current_ua = current_ua;
	b.temperature_hundredths = 2500;                /* the thermistor's raw code is not decoded: a neutral 25 C */
	b.present = (present & 1u) != 0;
	b.external = external;
	b.status = full ? 4 : charging ? 1 : external ? 3 : 2;   /* POWER_SUPPLY_STATUS: charging 1, discharging 2, not charging 3, full 4 */
	redwood_battery_report(&b);
	battery_soc_estimated = 1;
	static unsigned logs;
	if (logs++ % 30 == 0) {
		printf("battery: sid %u: %u mV from %#x (~%u%%, monotonic soc %u) %d uA usb %#x path %#x chgr %#x err %#x present %#x "
		    "-> external=%d charging=%d full=%d\n", sid, mv, haveV ? vregs[which] : 0, percent, msoc, (int)current_ua, usb, path,
		    charger, error, present, external, charging, full);
	}
}

static void
sapphire_battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		sapphire_battery_poll();
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

static void
sapphire_battery_init(void)
{
	thread_t thread;
	uint32_t version;

	(void)PE_parse_boot_argn("battery_sid", &sp_sid, sizeof(sp_sid));
	sp_core = ml_io_map(SAPPHIRE_ARB_CORE, 0x1100UL);
	sp_obs = ml_io_map(SAPPHIRE_ARB_OBSERVER, 0x10000UL);
	(void)SAPPHIRE_ARB_CNFG;
	if (sp_core == 0 || sp_obs == 0) {
		printf("battery: cannot map the PMIC arbiter\n");
		sp_obs = 0;
		return;
	}
	version = *(volatile uint32_t *)(sp_core + 0x000);
	sp_channels = *(volatile uint32_t *)(sp_core + 0x004) & 0x7ff;
	if (version < 0x50000000u || version >= 0x70000000u || sp_channels == 0) {
		printf("battery: PMIC arbiter version %#x, %u channels: only version 5 is known; no battery\n", version, sp_channels);
		sp_obs = 0;
		return;
	}
	if (sp_channels > 512) {
		sp_channels = 512;
	}
	printf("battery: sapphire: PMIC arbiter version %#x, %u channels, PM7250B on slave %u; polling every 5 seconds\n",
	    version, sp_channels, sp_sid);
	if (kernel_thread_start(sapphire_battery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
battery_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (unsigned tick = 0;; tick++) {
		battery_poll();
		if (tick % 12 == 0) {
			printf("battery: SOC=%d%%%s, V=%d mV, I=%d mA, T=%d C, battery=%d charger=%d charging=%d "
			    "action=%d (%llu reads, %llu failed)\n", battery_soc_percent,
			    battery_soc_estimated ? " (voltage estimate)" : "", battery_voltage_mv,
			    battery_current_ma, battery_temperature_c, battery_present, battery_charger_present,
			    battery_charging, battery_charger_action, battery_updates, battery_failures);
		}
		delay_for_interval(5000, NSEC_PER_MSEC);
	}
}

void
redwood_battery_init(void)
{
	uint32_t enabled = 0;
	uint64_t shared_pa = 0;
	vm_offset_t page;
	thread_t thread;

	if (!PE_parse_boot_argn("battery", &enabled, sizeof(enabled)) || !enabled) return;
	if (enabled == 2) {             /* the Redmi Note 13 4G: SPMI, no loader cache */
		sapphire_battery_init();
		return;
	}
	if (!PE_parse_boot_argn("battery_shared_pa", &shared_pa, sizeof(shared_pa)) || shared_pa == 0) {
		printf("battery: xnuloader did not provide battery_shared_pa\n");
		return;
	}
	page = ml_io_map((vm_offset_t)(shared_pa & ~(LOADER_PAGE_SIZE - 1)), LOADER_PAGE_SIZE);
	shared_battery = (volatile struct loader_battery *)(page + (shared_pa & (LOADER_PAGE_SIZE - 1)));
	if (shared_battery->magic != LOADER_BATTERY_MAGIC || shared_battery->version != LOADER_BATTERY_VERSION ||
	    shared_battery->size < sizeof(struct loader_battery)) {
		printf("battery: bad xnuloader cache at %#llx (magic %#llx version %u size %u)\n", shared_pa,
		    shared_battery->magic, shared_battery->version, shared_battery->size);
		return;
	}
	printf("battery: Qualcomm UEFI cache at pa %#llx; polling every 5 seconds\n", shared_pa);
	if (kernel_thread_start(battery_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
