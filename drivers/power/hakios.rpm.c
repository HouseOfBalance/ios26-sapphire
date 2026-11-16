/*
 * The RPM of the Redmi Note 13 4G (sapphire, SM6225 / "Bengal"): the resource and power manager the PMIC's
 * supplies, the memory's bus clock and the buses are voted through. This SoC has no RPMh (no command database,
 * no RSC: those are the POCO X5 Pro's, SC7280); it has the older RPM, reached as mainline's qcom_glink_rpm.c
 * and smd-rpm.c do it: two FIFOs in the RPM's message RAM, found through the table of contents at the end of
 * it, the APCS doorbell, and one intentless GLINK channel, "rpm_requests", over which "req" packets of
 * key-value pairs go. The audio codec's supply, the GPU's CX level, the Wi-Fi radio's and the modem's votes
 * all go through it, so it is here once, with one lock, for whoever needs it.
 *
 * Ported from the iOS 16 port of this phone (TopazModem.cpp, patches 0195, 0202, 0203, 0204, 0248, which has
 * run on the hardware): the transport and the two packets are its own; what is new is the lock, the names
 * and the boot-args.
 *
 *   hakios_rpm_vote(set, type, id, key, value)  one key-value pair to a resource: 0 ok, < 0 not (below)
 *   hakios_rpm_ldo(id, uv, ma, set)             an LDO of the PM6125 on at uv for ma (the RPM's "ldoa")
 *   set 0 is the active set, 1 the sleep set. The first caller starts the client.
 *
 * Boot-args: rpm=1 starts the client at boot and logs what it found (message RAM, FIFOs, version, channel);
 * rpm_ddr_khz=N then votes the DDR (BIMC, "clk2" id 0, key "KHz") for that many kHz -- what the RPMh vote of
 * hakios.cpufreq.c did on the other phone. Nothing else is touched unless a driver asks.
 *
 * Results: -3 the FIFO is full, -4 no answer in a second, -2 refused by the RPM, -1 an answer that is not "req",
 * -5 the client could not be started (no message RAM, no table of contents, no channel).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned milliseconds);

int hakios_rpm_vote(uint32_t set, uint32_t type, uint32_t id, uint32_t key, uint32_t value);
int hakios_rpm_ldo(uint32_t id, uint32_t uv, uint32_t ma, uint32_t set);

#define RPM_MSG_RAM_PA  0x045F0000ull           /* the RPM's message RAM; the table of contents is its last 256 bytes */
#define RPM_MSG_RAM_SZ  0x7000u
#define APCS_IPC_PA     0x0F111000ull           /* +8: the IPC register, bit 0 the RPM */
#define RPM_TOC_MAGIC   0x67727430u             /* "grt0" */
#define RPM_PIPE_RX     0x72326170u             /* "r2ap": the RPM to us */
#define RPM_PIPE_TX     0x61703272u             /* "ap2r": us to the RPM */

#define CMD_VERSION     0
#define CMD_VERSION_ACK 1
#define CMD_OPEN        2
#define CMD_CLOSE       3
#define CMD_OPEN_ACK    4
#define CMD_INTENT      5
#define CMD_TX_DATA     9
#define CMD_CLOSE_ACK   11
#define CMD_TX_DATA_CONT 12
#define CMD_READ_NOTIF  13

#define RPM_REQ         0x00716572u             /* "req" */
#define RPM_ERR         0x00727265u             /* "err" */

struct glink_msg {
	uint16_t cmd, p1;
	uint32_t p2;
} __attribute__((packed));

struct rpm_pipe {
	volatile uint32_t *tail, *head;
	volatile uint8_t *fifo;
	uint32_t len;
};

static struct rpm_pipe rpm_rx, rpm_tx;
static volatile uint8_t *rpm_apcs;
static uint32_t rpm_msg_id = 1;
static bool rpm_version, rpm_remote_open, rpm_open_sent, rpm_open_acked;
static int rpm_status = 1;                      /* the last answer: 1 none yet, 0 ok, < 0 refused */
static uint8_t rpm_buf[256];
static uint32_t rpm_len;

static LCK_GRP_DECLARE(rpm_lck_grp, "hakios-rpm");
static LCK_MTX_DECLARE(rpm_lock, &rpm_lck_grp);

#define mb() __asm__ volatile ("dmb sy" ::: "memory")

/* The FIFOs: whole 32-bit words, wrapping at the FIFO's length. */
static void
rpm_peek(uint32_t off, void *dst, uint32_t n)
{
	uint32_t t = *rpm_rx.tail + off;

	while (t >= rpm_rx.len) {
		t -= rpm_rx.len;
	}
	for (uint32_t i = 0; i < n; i += 4) {
		((uint32_t *)dst)[i / 4] = *(volatile uint32_t *)(rpm_rx.fifo + t);
		t += 4;
		if (t >= rpm_rx.len) {
			t = 0;
		}
	}
}

static void
rpm_advance(uint32_t n)
{
	uint32_t t = *rpm_rx.tail + n;

	while (t >= rpm_rx.len) {
		t -= rpm_rx.len;
	}
	mb();
	*rpm_rx.tail = t;
}

static bool
rpm_write(const void *hdr, uint32_t hlen, const void *data, uint32_t dlen)
{
	uint32_t w[80] = { 0 };
	uint32_t total = (hlen + dlen + 7) & ~7u;
	uint32_t head, tail, avail;

	if (total > sizeof(w)) {
		return false;
	}
	memcpy(w, hdr, hlen);
	if (dlen != 0) {
		memcpy((uint8_t *)w + hlen, data, dlen);
	}
	head = *rpm_tx.head;
	tail = *rpm_tx.tail;
	avail = tail <= head ? rpm_tx.len - head + tail : tail - head;
	if (avail <= total) {
		printf("rpm: TX FIFO full (head %x tail %x)\n", head, tail);
		return false;
	}
	for (uint32_t i = 0; i < total; i += 4) {
		*(volatile uint32_t *)(rpm_tx.fifo + head) = w[i / 4];
		head += 4;
		if (head >= rpm_tx.len) {
			head = 0;
		}
	}
	mb();
	*rpm_tx.head = head;
	mb();
	*(volatile uint32_t *)(rpm_apcs + 8) = 1;       /* the doorbell */
	return true;
}

static void
rpm_cmd(uint16_t cmd, uint16_t p1, uint32_t p2)
{
	struct glink_msg m = { cmd, p1, p2 };

	(void)rpm_write(&m, sizeof(m), NULL, 0);
}

static void
rpm_open(void)
{
	struct {
		struct glink_msg m;
		char name[16];
	} __attribute__((packed)) req;

	memset(&req, 0, sizeof(req));
	req.m.cmd = CMD_OPEN;
	req.m.p1 = 1;                                   /* our lcid */
	req.m.p2 = sizeof("rpm_requests");
	memcpy(req.name, "rpm_requests", sizeof("rpm_requests"));
	(void)rpm_write(&req, sizeof(req), NULL, 0);
	rpm_open_sent = true;
}

/* An answer: "req" holding "msg#" and, when refused, "err" with a string. */
static void
rpm_answer(const uint8_t *p, uint32_t len)
{
	const uint32_t *h = (const uint32_t *)p;
	int st = 0;
	uint32_t off = 8, end;

	if (len < 8 || h[0] != RPM_REQ) {
		printf("rpm: answer of %u bytes, not \"req\" (%08x)\n", len, len >= 4 ? h[0] : 0);
		rpm_status = -1;
		return;
	}
	end = MIN(len, 8 + h[1]);
	while (off + 8 <= end) {
		uint32_t type = *(const uint32_t *)(p + off), mlen = *(const uint32_t *)(p + off + 4);

		if (type == RPM_ERR) {
			char s[40] = { 0 };

			memcpy(s, p + off + 8, MIN(mlen, (uint32_t)sizeof(s) - 1));
			printf("rpm: refused: \"%s\"\n", s);
			st = -2;
		}
		off += (8 + mlen + 3) & ~3u;
	}
	rpm_status = st;
}

static bool
rpm_rx_one(void)
{
	uint32_t h = *rpm_rx.head, t = *rpm_rx.tail;
	uint32_t avail = h >= t ? h - t : rpm_rx.len - t + h, n;
	struct glink_msg m;

	if (avail < 8) {
		return false;
	}
	rpm_peek(0, &m, 8);
	switch (m.cmd) {
	case CMD_VERSION:
		rpm_advance(8);
		printf("rpm: glink VERSION %u features %x\n", m.p1, m.p2);
		rpm_cmd(CMD_VERSION_ACK, 1, 0);
		rpm_version = true;
		break;
	case CMD_VERSION_ACK:
		rpm_advance(8);
		rpm_version = true;
		break;
	case CMD_OPEN: {
		uint32_t name[8] = { 0 };
		bool ours;

		n = (8 + m.p2 + 7) & ~7u;
		if (avail < n) {
			return false;
		}
		rpm_peek(8, name, MIN((m.p2 + 3) & ~3u, (uint32_t)sizeof(name) - 4));
		rpm_advance(n);
		ours = strcmp((const char *)name, "rpm_requests") == 0;
		printf("rpm: glink OPEN \"%s\" rcid %u%s\n", (const char *)name, m.p1, ours ? "" : " (not ours)");
		if (ours) {
			rpm_remote_open = true;
			rpm_cmd(CMD_OPEN_ACK, m.p1, 0);
			if (!rpm_open_sent) {
				rpm_open();
			}
		}
		break;
	}
	case CMD_OPEN_ACK:
		rpm_advance(8);
		rpm_open_acked = true;
		break;
	case CMD_CLOSE:
		rpm_advance(8);
		printf("rpm: glink CLOSE rcid %u\n", m.p1);
		rpm_cmd(CMD_CLOSE_ACK, m.p1, 0);
		rpm_remote_open = false;
		break;
	case CMD_INTENT:
		n = (8 + 8 * m.p2 + 7) & ~7u;
		if (avail < n) {
			return false;
		}
		rpm_advance(n);
		break;
	case CMD_TX_DATA:
	case CMD_TX_DATA_CONT: {
		uint32_t cl[2];

		if (avail < 16) {
			return false;
		}
		rpm_peek(8, cl, 8);
		n = (16 + cl[0] + 7) & ~7u;
		if (avail < n) {
			return false;
		}
		if (m.cmd == CMD_TX_DATA) {
			rpm_len = 0;
		}
		if (rpm_len + cl[0] <= sizeof(rpm_buf) - 4) {
			rpm_peek(16, rpm_buf + rpm_len, (cl[0] + 3) & ~3u);
			rpm_len += cl[0];
		}
		rpm_advance(n);
		if (cl[1] == 0) {                       /* nothing left of this message */
			rpm_answer(rpm_buf, rpm_len);
		}
		break;
	}
	case CMD_READ_NOTIF:
		rpm_advance(8);
		*(volatile uint32_t *)(rpm_apcs + 8) = 1;
		break;
	default:
		rpm_advance(8);
		printf("rpm: glink cmd %u (%x %x)\n", m.cmd, m.p1, m.p2);
		break;
	}
	return true;
}

static bool rpm_is_up(void) { return rpm_remote_open && rpm_open_acked; }
static bool rpm_answered(void) { return rpm_status != 1; }

static void
rpm_poll(unsigned ms, bool (*done)(void))
{
	for (unsigned i = 0; i < ms && !done(); i++) {
		while (rpm_rx_one()) {
		}
		if (!done()) {
			IOSleep(1);
		}
	}
}

static bool
rpm_start(void)
{
	volatile uint8_t *ram = (volatile uint8_t *)ml_io_map(RPM_MSG_RAM_PA, RPM_MSG_RAM_SZ);
	volatile uint32_t *toc;

	rpm_apcs = (volatile uint8_t *)ml_io_map(APCS_IPC_PA, 0x1000);
	if (ram == NULL || rpm_apcs == NULL) {
		printf("rpm: mapping failed\n");
		return false;
	}
	toc = (volatile uint32_t *)(ram + RPM_MSG_RAM_SZ - 256);
	if (toc[0] != RPM_TOC_MAGIC) {
		printf("rpm: no table of contents (magic %08x)\n", toc[0]);
		return false;
	}
	for (uint32_t i = 0; i < toc[1] && i < 20; i++) {
		uint32_t id = toc[2 + 3 * i], off = toc[3 + 3 * i], size = toc[4 + 3 * i];
		struct rpm_pipe *p = id == RPM_PIPE_RX ? &rpm_rx : id == RPM_PIPE_TX ? &rpm_tx : NULL;

		if (p != NULL && off + size <= RPM_MSG_RAM_SZ) {
			p->tail = (volatile uint32_t *)(ram + off);
			p->head = (volatile uint32_t *)(ram + off + 4);
			p->fifo = ram + off + 8;
			p->len = size;
		}
	}
	if (rpm_rx.fifo == NULL || rpm_tx.fifo == NULL) {
		printf("rpm: FIFOs not in the table (%u entries)\n", toc[1]);
		return false;
	}
	printf("rpm: RX FIFO %u bytes (head %x tail %x), TX %u bytes (head %x tail %x)\n", rpm_rx.len, *rpm_rx.head,
	    *rpm_rx.tail, rpm_tx.len, *rpm_tx.head, *rpm_tx.tail);
	*rpm_tx.head = 0;                               /* as Linux: a fresh link */
	*rpm_rx.tail = 0;
	mb();
	rpm_cmd(CMD_VERSION, 1, 0);
	rpm_poll(1000, rpm_is_up);
	if (!rpm_is_up() && rpm_version && !rpm_open_sent) {
		rpm_open();                             /* (the RPM may still hold the channel open from the boot loader) */
		rpm_poll(500, rpm_is_up);
	}
	printf("rpm: version %s, rpm_requests %s\n", rpm_version ? "agreed" : "NO ANSWER", rpm_is_up() ? "open" : "NOT open");
	return rpm_open_acked;
}

/* One "req" packet with one resource and `pairs` key-value pairs of four bytes; the answer is waited for. */
static int
rpm_send(uint32_t set, uint32_t type, uint32_t id, const uint32_t *kv, unsigned pairs)
{
	struct {
		struct glink_msg m;
		uint32_t chunk, left;
		uint32_t svc, len, msg_id, flags, type, rid, dlen;
		uint32_t kvp[9];
	} __attribute__((packed)) pkt;
	uint32_t kbytes = pairs * 12;

	if (pairs == 0 || pairs > 3) {
		return -3;
	}
	memset(&pkt, 0, sizeof(pkt));
	pkt.m.cmd = CMD_TX_DATA;
	pkt.m.p1 = 1;
	pkt.m.p2 = 0;                                   /* intentless */
	pkt.chunk = 8 + 20 + kbytes;
	pkt.svc = RPM_REQ;
	pkt.len = 20 + kbytes;
	pkt.msg_id = rpm_msg_id++;
	pkt.flags = set;
	pkt.type = type;
	pkt.rid = id;
	pkt.dlen = kbytes;
	for (unsigned i = 0; i < pairs; i++) {
		pkt.kvp[3 * i] = kv[2 * i];             /* key */
		pkt.kvp[3 * i + 1] = 4;                 /* value length */
		pkt.kvp[3 * i + 2] = kv[2 * i + 1];     /* value */
	}
	rpm_status = 1;
	if (!rpm_write(&pkt, sizeof(pkt) - (9 - 3 * pairs) * 4, NULL, 0)) {
		return -3;
	}
	rpm_poll(1000, rpm_answered);
	return rpm_status == 1 ? -4 : rpm_status;
}

/*
 * One key-value pair to a resource (iOS 16 0202/0203): the smd-rpm types "clk0" misc (key "Enab": RPM clock
 * scaling), "clk1" bus (SNOC 2, CNOC 1), "clk2" memory (BIMC 0, the DDR), key "KHz"; "qup", "ipa", "mmxi", "ce".
 */
int
hakios_rpm_vote(uint32_t set, uint32_t type, uint32_t id, uint32_t key, uint32_t value)
{
	uint32_t kv[2] = { key, value };
	int r;

	lck_mtx_lock(&rpm_lock);
	r = (rpm_is_up() || rpm_start()) ? rpm_send(set, type, id, kv, 1) : -5;
	lck_mtx_unlock(&rpm_lock);
	return r;
}

/* An LDO on at uv for ma: "swen" 1, "uv", "ma" of the RPM's "ldoa" resource (iOS 16 0195, 0248). */
int
hakios_rpm_ldo(uint32_t id, uint32_t uv, uint32_t ma, uint32_t set)
{
	uint32_t kv[6] = { 0x6e657773 /* "swen" */, 1, 0x00007675 /* "uv" */, uv, 0x0000616d /* "ma" */, ma };
	int r;

	lck_mtx_lock(&rpm_lock);
	r = (rpm_is_up() || rpm_start()) ? rpm_send(set, 0x616f646c /* "ldoa" */, id, kv, 3) : -5;
	lck_mtx_unlock(&rpm_lock);
	return r;
}

void
hakios_rpm_init(void)
{
	uint32_t enabled = 0, khz = 0;

	if (!PE_parse_boot_argn("rpm", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	lck_mtx_lock(&rpm_lock);
	if (!rpm_start()) {
		printf("rpm: not started\n");
		lck_mtx_unlock(&rpm_lock);
		return;
	}
	lck_mtx_unlock(&rpm_lock);
	if (PE_parse_boot_argn("rpm_ddr_khz", &khz, sizeof(khz)) && khz != 0) {
		int r = hakios_rpm_vote(0, 0x326b6c63 /* "clk2" */, 0, 0x007a484b /* "KHz" */, khz);

		printf("rpm: DDR (BIMC) %u kHz: %d\n", khz, r);
	}
}
