/* ====================================================================
 * hakios: direct on-screen framebuffer text console for Sapphire (v2)
 *  - font 8x16 scale 1x  -> 130 cot x 140 dong
 *  - mau theo tag ([kprintf]/[kdbg] cyan, [startup] vang, EXCEPTION do)
 *  - scroll that (shadow buffer + diff redraw, KHONG doc tu framebuffer)
 *  - lock-free: khong malloc/kmalloc/spinlock, chi dung BSS tinh
 *  - hk_con_dump_exception / hk_con_print_ptr / hk_con_print_dec /
 *    hk_con_strip_ptr cho viec bisect boot som
 * ==================================================================== */
extern const unsigned char iso_font[];

/* API cong khai (dung qua `extern` trong sleh.c / startup.c / pe_kprintf.c ...) */
void     hk_con_clear(void);
void     hk_con_putc(char c);
void     hk_con_puts(const char *s);
void     hk_con_log(const char *s);
void     hk_con_hex(uint64_t val);
void     hk_con_print_dec(uint32_t val);
void     hk_con_print_ptr(const char *tag, uint64_t val);
void     hk_con_red_ptr(const char *tag, uint64_t val);
uint64_t hk_con_strip_ptr(uint64_t p);
void     hk_con_dump_exception(uint64_t esr, uint64_t elr, uint64_t far, uint64_t sp);

#ifndef HK_FB_BASE
#define HK_FB_BASE   ((volatile uint32_t *)0xfffffff004000000ULL)
#endif

#define HK_FB_W      1080
#define HK_FB_H      2400
#define HK_GLYPH_W   8
#define HK_GLYPH_H   16
#define HK_COLS      130                 /* 130*8  = 1040 px */
#define HK_ROWS      140                 /* 140*16 = 2240 px */
#define HK_X0        20                  /* (1080-1040)/2    */
#define HK_Y0        60                  /* chua cho camera  */

/* ARGB8888 */
#define HK_C_WHITE   0x00FFFFFFu
#define HK_C_CYAN    0x0000FFFFu
#define HK_C_YELLOW  0x00FFFF00u
#define HK_C_RED     0x00FF2020u
#define HK_C_GREEN   0x0040FF40u
#define HK_C_BG      0x00000000u

enum { HK_COL_WHITE = 0, HK_COL_CYAN, HK_COL_YELLOW, HK_COL_RED, HK_COL_GREEN };
static const uint32_t hk_palette[] = {
	HK_C_WHITE, HK_C_CYAN, HK_C_YELLOW, HK_C_RED, HK_C_GREEN
};

#define HK_TAG(s, c)  { s, (uint8_t)(sizeof(s) - 1), c }
static const struct { const char *tag; uint8_t len; uint8_t col; } hk_tags[] = {
	HK_TAG("[kprintf]",          HK_COL_CYAN),
	HK_TAG("[kdbg]",             HK_COL_CYAN),
	HK_TAG("[startup]",          HK_COL_YELLOW),
	HK_TAG("=== EXCEPTION ===",  HK_COL_RED),
	HK_TAG("[*]",                HK_COL_GREEN),   /* xoa dong nay neu khong muon */
};
#define HK_NTAGS (sizeof(hk_tags) / sizeof(hk_tags[0]))
#define HK_MAXTAG 17

/* cell = (mau << 8) | ky tu ; o trong luon = 0x0020 */
static uint16_t hk_txt[HK_ROWS][HK_COLS];   /* noi dung can hien thi  */
static uint16_t hk_scr[HK_ROWS][HK_COLS];   /* nhung gi dang tren man */
static uint32_t hk_cx, hk_cy;
static uint8_t  hk_line_col;
static uint8_t  hk_ready;

static void hk_draw_cell(uint32_t cx, uint32_t cy, uint16_t cell)
{
	volatile uint32_t *fb = HK_FB_BASE;
	unsigned char ch = (unsigned char)(cell & 0xff);
	uint32_t fg = hk_palette[cell >> 8];
	const unsigned char *glyph;
	uint32_t base;

	if (ch < 32 || ch > 126) ch = ' ';
	glyph = &iso_font[ch * HK_GLYPH_H];
	base = (HK_Y0 + cy * HK_GLYPH_H) * HK_FB_W + HK_X0 + cx * HK_GLYPH_W;

	for (int row = 0; row < HK_GLYPH_H; row++) {
		unsigned char bits = glyph[row];
		volatile uint32_t *p = fb + base + row * HK_FB_W;
		for (int col = 0; col < HK_GLYPH_W; col++) {
			p[col] = (bits & (1u << col)) ? fg : HK_C_BG;
		}
	}
	hk_scr[cy][cx] = cell;
}

static void hk_sync_row(uint32_t r)
{
	for (uint32_t c = 0; c < HK_COLS; c++) {
		if (hk_txt[r][c] != hk_scr[r][c]) {
			hk_draw_cell(c, r, hk_txt[r][c]);
		}
	}
}

static void hk_scroll(void)
{
	for (uint32_t r = 1; r < HK_ROWS; r++) {
		for (uint32_t c = 0; c < HK_COLS; c++) {
			hk_txt[r - 1][c] = hk_txt[r][c];
		}
	}
	for (uint32_t c = 0; c < HK_COLS; c++) {
		hk_txt[HK_ROWS - 1][c] = 0x0020;
	}
	/* chi ve lai cac o thay doi (o trong giong nhau duoc bo qua) */
	for (uint32_t r = 0; r < HK_ROWS; r++) {
		hk_sync_row(r);
	}
}

static void hk_newline(void)
{
	hk_cx = 0;
	hk_line_col = HK_COL_WHITE;
	if (hk_cy + 1 >= HK_ROWS) {
		hk_scroll();
	} else {
		hk_cy++;
	}
}

void hk_con_clear(void)
{
	volatile uint32_t *fb = HK_FB_BASE;
	for (uint32_t i = 0; i < HK_FB_W * HK_FB_H; i++) {
		fb[i] = HK_C_BG;
	}
	for (uint32_t r = 0; r < HK_ROWS; r++) {
		for (uint32_t c = 0; c < HK_COLS; c++) {
			hk_txt[r][c] = 0x0020;
			hk_scr[r][c] = 0x0020;
		}
	}
	hk_cx = 0;
	hk_cy = 0;
	hk_line_col = HK_COL_WHITE;
	hk_ready = 1;
}

/* Nhan dien tag o dau dong, to mau ca dong (ke ca phan da in) */
static void hk_detect_tag(void)
{
	for (unsigned t = 0; t < HK_NTAGS; t++) {
		if (hk_tags[t].len != hk_cx) continue;
		uint32_t i;
		for (i = 0; i < hk_cx; i++) {
			if ((hk_txt[hk_cy][i] & 0xff) != (unsigned char)hk_tags[t].tag[i]) break;
		}
		if (i == hk_cx) {
			hk_line_col = hk_tags[t].col;
			for (i = 0; i < hk_cx; i++) {
				uint16_t ch = hk_txt[hk_cy][i] & 0xff;
				hk_txt[hk_cy][i] = (uint16_t)((ch == ' ' ? 0 : hk_line_col) << 8) | ch;
				hk_draw_cell(i, hk_cy, hk_txt[hk_cy][i]);
			}
			return;
		}
	}
}

void hk_con_putc(char c)
{
	if (!hk_ready) hk_con_clear();

	if (c == '\r') { hk_cx = 0; return; }
	if (c == '\n') { hk_newline(); return; }
	if (c == '\t') {
		uint32_t n = 8 - (hk_cx & 7);
		while (n--) hk_con_putc(' ');
		return;
	}
	if ((unsigned char)c < 32 || (unsigned char)c > 126) c = ' ';

	if (hk_cx >= HK_COLS) {
		hk_newline();                      /* tu xuong dong khi het cot */
	}

	uint16_t cell = (uint16_t)(((c == ' ') ? 0 : hk_line_col) << 8) | (unsigned char)c;
	hk_txt[hk_cy][hk_cx] = cell;
	hk_draw_cell(hk_cx, hk_cy, cell);
	hk_cx++;

	if (hk_cx <= HK_MAXTAG && hk_line_col == HK_COL_WHITE) {
		hk_detect_tag();
	}
}

void hk_con_puts(const char *s)
{
	if (!s) return;
	while (*s) {
		hk_con_putc(*s++);
	}
}

void hk_con_log(const char *s)
{
	hk_con_puts(s);
	hk_con_putc(10);
}

void hk_con_hex(uint64_t val)
{
	static const char hexchars[] = "0123456789abcdef";
	hk_con_puts("0x");
	for (int i = 60; i >= 0; i -= 4) {
		hk_con_putc(hexchars[(val >> i) & 0xf]);
	}
}

/* ====================================================================
 * Tien ich debug som (lock-free, khong format string)
 * ==================================================================== */
#if defined(__aarch64__)
#define HK_HALT()  for (;;) { __asm__ volatile ("wfe"); }
#else
#define HK_HALT()  return
#endif

void hk_con_print_dec(uint32_t val)
{
	char buf[10];
	int n = 0;

	if (val == 0) {
		hk_con_putc('0');
		return;
	}
	while (val) {
		buf[n++] = (char)('0' + (val % 10));
		val /= 10;
	}
	while (n) {
		hk_con_putc(buf[--n]);
	}
}

/* in "<tag>0x<16 hex>\n" */
void hk_con_print_ptr(const char *tag, uint64_t val)
{
	hk_con_puts(tag);
	hk_con_hex(val);
	hk_con_putc('\n');
}

/*
 * Bo PAC signature khoi con tro ham kernel. Dia chi kernel hop le co dang
 * 0xfffffff0_xxxxxxxx (36 bit thap la dia chi that, bit 63:36 phai bang 1).
 * Neu da canonical thi giu nguyen, neu khong thi giu 36 bit thap va ep
 * prefix 0xfffffff000000000.
 */
uint64_t hk_con_strip_ptr(uint64_t p)
{
	if ((p >> 36) == 0xfffffffULL) {
		return p;
	}
	return (p & 0x0000000FFFFFFFFFULL) | 0xfffffff000000000ULL;
}

/* bat dau mot dong moi (neu dang do dang) va ep mau do cho ca dong */
static void hk_red_line(void)
{
	if (hk_cx != 0) {
		hk_newline();
	}
	hk_line_col = HK_COL_RED;
}

void hk_con_red_ptr(const char *tag, uint64_t val)
{
	hk_red_line();
	hk_con_print_ptr(tag, val);
}

static const struct { uint8_t ec; const char *name; } hk_ec_names[] = {
	{ 0x00, "Unknown/UNDEF (PAC/LSE?)" },
	{ 0x01, "WFI/WFE trap" },
	{ 0x07, "FP/SIMD access" },
	{ 0x0e, "Illegal execution state" },
	{ 0x15, "SVC (AArch64)" },
	{ 0x18, "MSR/MRS/SYS trap" },
	{ 0x20, "Inst Abort (lower EL)" },
	{ 0x21, "Inst Abort (same EL)" },
	{ 0x22, "PC alignment" },
	{ 0x24, "Data Abort (lower EL)" },
	{ 0x25, "Data Abort (same EL)" },
	{ 0x26, "SP alignment" },
	{ 0x2f, "SError" },
	{ 0x3c, "BRK" },
};

/* Dump khan cap, mau do. Goi duoc tu vector/sleh truoc moi logic khac. */
void hk_con_dump_exception(uint64_t esr, uint64_t elr, uint64_t far, uint64_t sp)
{
	static uint8_t hk_in_dump;
	uint32_t ec = (uint32_t)((esr >> 26) & 0x3f);
	uint32_t iss = (uint32_t)(esr & 0x1ffffff);
	const char *name = "?";

	if (hk_in_dump) {
		HK_HALT();      /* dump bi fault long nhau: giu nguyen man hinh */
	}
	hk_in_dump = 1;

	for (unsigned i = 0; i < sizeof(hk_ec_names) / sizeof(hk_ec_names[0]); i++) {
		if (hk_ec_names[i].ec == ec) {
			name = hk_ec_names[i].name;
			break;
		}
	}

	hk_red_line();
	hk_con_puts("=== EXCEPTION === EL1 SYNC");
	hk_con_putc('\n');

	hk_con_red_ptr("ESR: ", esr);

	hk_red_line();
	hk_con_puts("EC : ");
	hk_con_print_dec(ec);
	hk_con_puts(" ");
	hk_con_puts(name);
	if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
		hk_con_puts(" FSC=");
		hk_con_print_dec(iss & 0x3f);
		if (ec == 0x24 || ec == 0x25) {
			hk_con_puts(" WnR=");
			hk_con_print_dec((iss >> 6) & 1);
		}
	}
	hk_con_putc('\n');

	hk_con_red_ptr("ELR: ", elr);
	hk_con_red_ptr("FAR: ", far);
	hk_con_red_ptr("SP : ", sp);

	hk_in_dump = 0;
}
