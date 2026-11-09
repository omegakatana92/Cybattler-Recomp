/* Cybattler machine: memory maps, interrupts, scheduler, I/O MCU HLE,
   ROM loading, save states. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "recomp_common.h"
#include "miniz.h"

roms_t g_roms;
mstate_t g_m;
int g_use_recomp = 1;

static int g_cur;                 /* 0 = main, 1 = sound (memory map selector) */
static int g_running = -1;        /* CPU whose context is live in Musashi */
static void *g_ctx[2];
static int g_budget[2];
static uint8_t g_ym_blob[4096];
FILE *g_dbg;

#define STATE_VERSION 1

/* ------------------------------------------------------------------ IRQs */
static int highest(uint8_t m) { for (int l = 7; l > 0; l--) if (m & (1 << l)) return l; return 0; }
static uint8_t *irqmask(int cpu) { return cpu ? &g_m.irq_snd : &g_m.irq_main; }

static void raise_irq(int cpu, int level)
{
	*irqmask(cpu) |= 1 << level;
	if (cpu == g_running) m68k_set_irq(highest(*irqmask(cpu)));
}

void mach_snd_irq(int level) { raise_irq(1, level); }

int mach_int_ack(int level)
{
	uint8_t *m = irqmask(g_cur);
	*m &= ~(1 << level);
	m68k_set_irq(highest(*m));
	return M68K_INT_ACK_AUTOVECTOR;
}

/* ------------------------------------------------------- I/O MCU (HLE) */
/* Codes the game sends to the TMP91640 and what comes back. Order:
   coins, P1, P2, DSW1, DSW2, startup-check, handshake. The startup check
   expects 0x0d so that f(f(0x0d)) == 6 with f(x) = (x*x >> 4) & 0xff. */
static const uint8_t ip_codes[7] = { 0x56, 0x52, 0x53, 0x54, 0x55, 0xf2, 0x06 };

static void ip_select_w(uint16_t data)
{
	int i;
	for (i = 0; i < 7; i++) if ((data & 0xff) == ip_codes[i]) break;
	switch (i) {
	case 0: g_m.ip_latched = 0xff00 | g_m.in_sys; break;
	case 1: g_m.ip_latched = g_m.in_p1; break;
	case 2: g_m.ip_latched = 0xff00 | g_m.in_p2; break;
	case 3: g_m.ip_latched = g_m.dsw1; break;
	case 4: g_m.ip_latched = g_m.dsw2; break;
	case 5: g_m.ip_latched = 0x0d; break;
	case 6: g_m.ip_latched = 0x06; break;
	default: return;
	}
	raise_irq(0, 2);
}

/* ---------------------------------------------------------- main CPU map */
static void tmap_w(int L, int reg, uint16_t data, uint16_t mask)
{
	uint16_t *r = reg == 0 ? &g_m.tmap[L].scrollx : reg == 1 ? &g_m.tmap[L].scrolly : &g_m.tmap[L].flag;
	*r = (*r & ~mask) | (data & mask);
}
static uint16_t tmap_r(int L, int reg)
{
	return reg == 0 ? g_m.tmap[L].scrollx : reg == 1 ? g_m.tmap[L].scrolly : reg == 2 ? g_m.tmap[L].flag : 0xffff;
}

static void screen_flag_w(uint16_t v)
{
	uint16_t old = g_m.screen_flag;
	g_m.screen_flag = v;
	if (v & 0x10) {
		g_m.snd_in_reset = 1;
		ym_reset();
		oki_reset(0); oki_reset(1);
	} else if (old & 0x10) {
		g_m.snd_in_reset = 2;  /* released: reset pulse before it runs again */
	}
}

static uint16_t main_r16(uint32_t a)
{
	a &= 0x1ffffe;
	if (a < 0x80000) return (g_roms.main_rom[a] << 8) | g_roms.main_rom[a + 1];
	if (a >= 0x1c0000) return g_m.ram[(a & 0xffff) >> 1];
	if (a >= 0x0e0000 && a < 0x0f8000) {
		int L = (a - 0x0e0000) >> 15;
		return g_m.scrollram[L][(a & 0x3fff) >> 1];
	}
	if (a >= 0x0f8000 && a < 0x0f8800) return g_m.palette[(a & 0x7ff) >> 1];
	if (a >= 0x0d2000 && a < 0x0d4000) return g_m.objram[(a & 0x1fff) >> 1];
	switch (a) {
	case 0x0c2000: case 0x0c2002: case 0x0c2004: return tmap_r(0, (a - 0x0c2000) >> 1);
	case 0x0c2008: case 0x0c200a: case 0x0c200c: return tmap_r(1, (a - 0x0c2008) >> 1);
	case 0x0c2100: case 0x0c2102: case 0x0c2104: return tmap_r(2, (a - 0x0c2100) >> 1);
	case 0x0c2200: return g_m.sprite_flag;
	case 0x0c8000: return g_m.latch_to_main;
	case 0x0d8000: return g_m.ip_latched;
	}
	return 0;
}

static void main_w16(uint32_t a, uint16_t d, uint16_t mask)
{
	a &= 0x1ffffe;
	if (a >= 0x1c0000) {
		/* byte writes land in both halves (MAME ram_w, needed for attract inputs) */
		g_m.ram[(a & 0xffff) >> 1] = d;
		return;
	}
#define COMB(p) (p) = (uint16_t)(((p) & ~mask) | (d & mask))
	if (a >= 0x0e0000 && a < 0x0f8000) {
		int L = (a - 0x0e0000) >> 15;
		COMB(g_m.scrollram[L][(a & 0x3fff) >> 1]);
		return;
	}
	if (a >= 0x0f8000 && a < 0x0f8800) { COMB(g_m.palette[(a & 0x7ff) >> 1]); return; }
	if (a >= 0x0d2000 && a < 0x0d4000) { COMB(g_m.objram[(a & 0x1fff) >> 1]); return; }
	switch (a) {
	case 0x0c2000: case 0x0c2002: case 0x0c2004: tmap_w(0, (a - 0x0c2000) >> 1, d, mask); return;
	case 0x0c2008: case 0x0c200a: case 0x0c200c: tmap_w(1, (a - 0x0c2008) >> 1, d, mask); return;
	case 0x0c2100: case 0x0c2102: case 0x0c2104: tmap_w(2, (a - 0x0c2100) >> 1, d, mask); return;
	case 0x0c2108: COMB(g_m.sprite_bank); return;
	case 0x0c2200: COMB(g_m.sprite_flag); return;
	case 0x0c2208: COMB(g_m.active_layers); return;
	case 0x0c2308: { uint16_t v = g_m.screen_flag; COMB(v); screen_flag_w(v); return; }
	case 0x0c8000: g_m.latch_to_snd = d; raise_irq(1, 2); return;
	case 0x0d8000: ip_select_w(d); return;
	}
#undef COMB
}

/* --------------------------------------------------------- sound CPU map */
static uint16_t snd_r16(uint32_t a)
{
	a &= 0xfffffe;
	if (a < 0x20000) return (g_roms.snd_rom[a] << 8) | g_roms.snd_rom[a + 1];
	if (a >= 0x0e0000 && a < 0x100000) return g_m.snd_ram[(a & 0xffff) >> 1];
	if (a == 0x040000 || a == 0x060000) return g_m.latch_to_snd;
	if (a >= 0x080000 && a < 0x080004) return ym_read();
	return 0; /* OKI status reads return 0 (MAME ignore_oki_status) */
}

static void snd_w16(uint32_t a, uint16_t d, uint16_t mask)
{
	a &= 0xfffffe;
	if (a >= 0x0e0000 && a < 0x100000) {
		uint16_t *p = &g_m.snd_ram[(a & 0xffff) >> 1];
		*p = (uint16_t)((*p & ~mask) | (d & mask));
		return;
	}
	if (a == 0x040000 || a == 0x060000) { g_m.latch_to_main = d; return; }
	if (!(mask & 0x00ff)) return; /* chips are on the low byte lane */
	if (a >= 0x080000 && a < 0x080004) { ym_write((a >> 1) & 1, d & 0xff); return; }
	if (a >= 0x0a0000 && a < 0x0a0004) { oki_write(0, d & 0xff); return; }
	if (a >= 0x0c0000 && a < 0x0c0004) { oki_write(1, d & 0xff); return; }
}

/* ------------------------------------------------- Musashi memory hooks */
static uint16_t r16(uint32_t a) { return g_cur ? snd_r16(a) : main_r16(a); }
static void w16(uint32_t a, uint16_t d, uint16_t m) { if (g_cur) snd_w16(a, d, m); else main_w16(a, d, m); }

unsigned int m68k_read_memory_8(unsigned int a) { uint16_t w = r16(a); return (a & 1) ? (w & 0xff) : (w >> 8); }
unsigned int m68k_read_memory_16(unsigned int a) { return r16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return (r16(a) << 16) | r16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v)
{
	v &= 0xff;
	w16(a, (uint16_t)((v << 8) | v), (a & 1) ? 0x00ff : 0xff00);
}
void m68k_write_memory_16(unsigned int a, unsigned int v) { w16(a, (uint16_t)v, 0xffff); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { w16(a, (uint16_t)(v >> 16), 0xffff); w16(a + 2, (uint16_t)v, 0xffff); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return m68k_read_memory_8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ------------------------------------------------------------ ROM load */
typedef struct { const char *name; uint32_t crc; uint32_t size; } romfile_t;
static const romfile_t romlist[] = {
	{ "cb_03.rom", 0xbee20587, 0x40000 }, { "cb_02.rom", 0x2ed14c50, 0x40000 },
	{ "cb_08.rom", 0xbf7b3558, 0x10000 }, { "cb_07.rom", 0x85d219d7, 0x10000 },
	{ "cb_m01.rom", 0x1109337f, 0x80000 }, { "cb_m04.rom", 0x0c91798e, 0x80000 },
	{ "cb_09.rom", 0x37b1f195, 0x20000 }, { "cb_m03.rom", 0x4cd49f58, 0x80000 },
	{ "cb_m02.rom", 0x882825db, 0x80000 }, { "cb_11.rom", 0x59d62d1f, 0x40000 },
	{ "cb_10.rom", 0x8af95eed, 0x40000 }, { "pr-91028.12", 0xcfe90082, 0x200 },
};
#define NROMS (sizeof romlist / sizeof romlist[0])

static uint8_t *find_in_zip(mz_zip_archive *z, const char *name, size_t *sz)
{
	int n = (int)mz_zip_reader_get_num_files(z);
	for (int i = 0; i < n; i++) {
		char fn[512];
		mz_zip_reader_get_filename(z, i, fn, sizeof fn);
		const char *b = strrchr(fn, '/'); b = b ? b + 1 : fn;
		if (!strcasecmp(b, name)) return mz_zip_reader_extract_to_heap(z, i, sz, 0);
	}
	return NULL;
}

static uint8_t *load_file(const char *dir, const char *name, size_t *sz)
{
	char p[1024];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); *sz = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*sz);
	if (fread(b, 1, *sz, f) != *sz) { free(b); fclose(f); return NULL; }
	fclose(f);
	return b;
}

static void decode_tiles(const uint8_t *src, size_t bytes, uint8_t **out, unsigned *n)
{
	*n = (unsigned)(bytes / 32);
	*out = malloc((size_t)*n * 64);
	for (unsigned t = 0; t < *n; t++)
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++) {
				uint8_t b = src[t * 32 + y * 4 + x / 2];
				(*out)[t * 64 + y * 8 + x] = (x & 1) ? (b & 15) : (b >> 4);
			}
}

int machine_load_roms(const char *path)
{
	uint8_t *data[NROMS] = { 0 };
	size_t sizes[NROMS] = { 0 };
	mz_zip_archive z; memset(&z, 0, sizeof z);
	int is_zip = mz_zip_reader_init_file(&z, path, 0);
	int ok = 1;
	for (unsigned i = 0; i < NROMS; i++) {
		data[i] = is_zip ? find_in_zip(&z, romlist[i].name, &sizes[i]) : load_file(path, romlist[i].name, &sizes[i]);
		if (!data[i] || sizes[i] != romlist[i].size) {
			fprintf(stderr, "missing or wrong size: %s\n", romlist[i].name); ok = 0; continue;
		}
		uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, data[i], sizes[i]);
		if (crc != romlist[i].crc)
			fprintf(stderr, "warning: %s CRC %08x (expected %08x)\n", romlist[i].name, crc, romlist[i].crc);
	}
	if (is_zip) mz_zip_reader_end(&z);
	if (!ok) return 0;
	for (int i = 0; i < 0x40000; i++) {
		g_roms.main_rom[i * 2] = data[0][i]; g_roms.main_rom[i * 2 + 1] = data[1][i];
	}
	for (int i = 0; i < 0x10000; i++) {
		g_roms.snd_rom[i * 2] = data[2][i]; g_roms.snd_rom[i * 2 + 1] = data[3][i];
	}
	decode_tiles(data[4], 0x80000, &g_roms.tiles[0], &g_roms.ntiles[0]);
	decode_tiles(data[5], 0x80000, &g_roms.tiles[1], &g_roms.ntiles[1]);
	decode_tiles(data[6], 0x20000, &g_roms.tiles[2], &g_roms.ntiles[2]);
	{
		uint8_t *spr = malloc(0x100000), *t8; unsigned n8;
		memcpy(spr, data[7], 0x80000); memcpy(spr + 0x80000, data[8], 0x80000);
		decode_tiles(spr, 0x100000, &t8, &n8);
		g_roms.nsprites = n8 / 4;
		g_roms.sprites = malloc((size_t)g_roms.nsprites * 256);
		for (unsigned s = 0; s < g_roms.nsprites; s++)
			for (int q = 0; q < 4; q++)          /* column-major 2x2 group */
				for (int y = 0; y < 8; y++)
					for (int x = 0; x < 8; x++)
						g_roms.sprites[s * 256 + ((q & 1) * 8 + y) * 16 + (q >> 1) * 8 + x] =
							t8[(s * 4 + q) * 64 + y * 8 + x];
		free(t8); free(spr);
	}
	memcpy(g_roms.oki_rom[0], data[9], 0x40000);
	memcpy(g_roms.oki_rom[1], data[10], 0x40000);
	memcpy(g_roms.prom, data[11], 0x200);
	for (unsigned i = 0; i < NROMS; i++) free(data[i]);
	return 1;
}

/* ------------------------------------------------------- reset / frame */
static float ym_l[2048], ym_r[2048], oki_buf[2][1024];
static int ym_n, oki_n;
static double oki_acc;

void machine_reset(void)
{
	uint8_t in_sys = g_m.in_sys ? g_m.in_sys : 0xff;
	uint8_t d1 = g_m.dsw1 ? g_m.dsw1 : 0xff, d2 = g_m.dsw2 ? g_m.dsw2 : 0xbf;
	static int inited;
	memset(&g_m, 0, sizeof g_m);
	g_m.version = STATE_VERSION;
	g_m.in_sys = in_sys; g_m.in_p1 = 0xff; g_m.in_p2 = 0xff;
	g_m.dsw1 = d1; g_m.dsw2 = d2;
	g_m.ip_latched = 0x0006;
	for (int L = 0; L < 3; L++) for (int i = 0; i < 0x2000; i++) g_m.scrollram[L][i] = 0xffff;
	if (!inited) {
		m68k_init();
		g_ctx[0] = calloc(1, m68k_context_size());
		g_ctx[1] = calloc(1, m68k_context_size());
		ym_init(SND_CLOCK / 2);
		video_init();
		inited = 1;
	}
	ym_reset();
	oki_reset(0); oki_reset(1);
	for (int c = 1; c >= 0; c--) {
		g_cur = c;
		m68k_set_cpu_type(M68K_CPU_TYPE_68000);
		m68k_pulse_reset();
		m68k_get_context(g_ctx[c]);
		g_budget[c] = 0;
	}
	m68k_set_context(g_ctx[0]);
	g_cur = 0;
}

static void run_cpu(int c, int cycles)
{
	g_cur = c;
	m68k_set_context(g_ctx[c]);
	if (c == 1 && g_m.snd_in_reset == 2) {
		g_m.snd_in_reset = 0;
		g_m.irq_snd = 0;
		m68k_pulse_reset();
		g_budget[1] = 0;
	}
	g_running = c;
	m68k_set_irq(highest(*irqmask(c)));
	g_budget[c] += cycles;
	if (g_budget[c] > 0) {
		const rc_table *t = g_use_recomp ? (c ? &rc_snd_table : &rc_main_table) : NULL;
		g_budget[c] -= rc_execute(t, g_budget[c], c);
	}
	g_running = -1;
	if (g_dbg) {
		fprintf(g_dbg, "%u %d %d pc=%06x sr=%04x", g_m.frame, g_m.line, c, REG_PC, m68ki_get_sr());
		for (int i = 0; i < 16; i++) fprintf(g_dbg, " %08x", REG_DA[i]);
		fprintf(g_dbg, " b=%d\n", g_budget[c]);
	}
	m68k_get_context(g_ctx[c]);
}

void machine_run_frame(void)
{
	ym_n = 0; oki_n = 0;
	for (int line = 0; line < LINES_PER_FRAME; line++) {
		g_m.line = line;
		if (line == VIS_FIRST) { raise_irq(0, 2); video_begin_frame(); }
		if (line == 80 + 16) raise_irq(0, 1);
		if (line == 224 + 16) { raise_irq(0, 4); video_vblank(); }
		if (line >= VIS_FIRST && line <= VIS_LAST) video_render_line(line - VIS_FIRST);

		run_cpu(0, MAIN_CYC_PER_LINE);
		if (g_m.snd_in_reset != 1) run_cpu(1, SND_CYC_PER_LINE);
		g_cur = 0;

		int n;
		ym_advance(SND_CLOCK / 2 / (PIXEL_CLOCK / HTOTAL), ym_l + ym_n, ym_r + ym_n, &n);
		ym_n += n;
		oki_acc += (4000000.0 / 132.0) / ((double)PIXEL_CLOCK / HTOTAL);
		int on = (int)oki_acc; oki_acc -= on;
		if (on) { oki_render(0, oki_buf[0] + oki_n, on); oki_render(1, oki_buf[1] + oki_n, on); oki_n += on; }
	}
	m68k_set_context(g_ctx[0]);
	g_m.frame++;
}

const uint32_t *machine_framebuffer(void) { return video_fb(); }

/* Resample this frame's audio to `rate` and return stereo int16 frames */
int machine_audio(int16_t *out, int max_frames, int rate)
{
	static double acc;
	acc += rate / FRAME_HZ;
	int n = (int)acc; acc -= n;
	if (n > max_frames) n = max_frames;
	for (int i = 0; i < n; i++) {
		double t = (double)i / n;
		float yl = 0, yr = 0, ok = 0;
		if (ym_n) { double p = t * ym_n; int k = (int)p; float f = (float)(p - k); int k2 = k + 1 < ym_n ? k + 1 : k;
			yl = ym_l[k] + (ym_l[k2] - ym_l[k]) * f; yr = ym_r[k] + (ym_r[k2] - ym_r[k]) * f; }
		if (oki_n) { double p = t * oki_n; int k = (int)p; float f = (float)(p - k); int k2 = k + 1 < oki_n ? k + 1 : k;
			ok = (oki_buf[0][k] + (oki_buf[0][k2] - oki_buf[0][k]) * f) + (oki_buf[1][k] + (oki_buf[1][k2] - oki_buf[1][k]) * f); }
		/* MAME route gains (0.80 FM, 0.30 per OKI), then -6 dB headroom and a soft knee */
		float l = (yl * 0.80f + ok * 0.30f) * 0.5f, r = (yr * 0.80f + ok * 0.30f) * 0.5f;
		l = l > 0.75f ? 0.75f + (l - 0.75f) / (1 + (l - 0.75f) * 4) : l < -0.75f ? -0.75f + (l + 0.75f) / (1 - (l + 0.75f) * 4) : l;
		r = r > 0.75f ? 0.75f + (r - 0.75f) / (1 + (r - 0.75f) * 4) : r < -0.75f ? -0.75f + (r + 0.75f) / (1 - (r + 0.75f) * 4) : r;
		if (l > 1) l = 1; if (l < -1) l = -1; if (r > 1) r = 1; if (r < -1) r = -1;
		out[i * 2] = (int16_t)(l * 32767); out[i * 2 + 1] = (int16_t)(r * 32767);
	}
	return n;
}

/* ---------------------------------------------------------- save states */
int machine_save_state(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	int sz = (int)m68k_context_size();
	int ysz = ym_save(g_ym_blob, sizeof g_ym_blob);
	fwrite("CYBS", 1, 4, f);
	fwrite(&g_m, sizeof g_m, 1, f);
	fwrite(&sz, 4, 1, f); fwrite(g_ctx[0], sz, 1, f); fwrite(g_ctx[1], sz, 1, f);
	fwrite(g_budget, sizeof g_budget, 1, f);
	fwrite(&ysz, 4, 1, f); fwrite(g_ym_blob, ysz, 1, f);
	fclose(f);
	return 1;
}

int machine_load_state(const char *path)
{
	FILE *f = fopen(path, "rb");
	char magic[4]; int sz, ysz; int ok = 0;
	static mstate_t tmp;
	if (!f) return 0;
	if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "CYBS", 4) &&
	    fread(&tmp, sizeof tmp, 1, f) == 1 && tmp.version == STATE_VERSION &&
	    fread(&sz, 4, 1, f) == 1 && sz == (int)m68k_context_size()) {
		g_m = tmp;
		ok = fread(g_ctx[0], sz, 1, f) == 1 && fread(g_ctx[1], sz, 1, f) == 1 &&
		     fread(g_budget, sizeof g_budget, 1, f) == 1 &&
		     fread(&ysz, 4, 1, f) == 1 && ysz > 0 && ysz <= (int)sizeof g_ym_blob &&
		     fread(g_ym_blob, ysz, 1, f) == 1;
		if (ok) ym_load(g_ym_blob, ysz);
		m68k_set_context(g_ctx[0]); g_cur = 0; g_running = -1;
	}
	fclose(f);
	return ok;
}
