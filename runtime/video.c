/* Mega System 1 type C video: 3 scroll layers + object/sprite system,
   priority PROM. Behaviour follows MAME's megasys1_v.cpp / ms1_tmap.cpp
   (BSD-3-Clause, Luca Elia, David Haywood) but renders per scanline. */
#include <string.h>
#include "machine.h"

static uint32_t fb[SCREEN_W * SCREEN_H];
static uint16_t spr_bm[256 * 256];      /* sprite framebuffer, bitmap coords */
static uint32_t layers_order[16];
static uint32_t pal_rgb[0x400];

uint32_t *video_fb(void) { return fb; }

/* ---- priority PROM conversion (straight port of priority_create) ---- */
static void priority_create(void)
{
	const uint8_t *pri_prom = g_roms.prom;
	for (int pri_code = 0; pri_code < 0x10; pri_code++) {
		uint32_t lo[2];
		for (int offset = 0; offset < 2; offset++) {
			uint8_t enable_mask = 0xf;
			lo[offset] = 0xfffff;
			do {
				uint8_t top = pri_prom[pri_code * 0x20 + offset + enable_mask * 2] & 3;
				uint8_t top_mask = 1 << top;
				uint8_t result = 0;
				for (int i = 0; i < 0x10; i++) {
					uint8_t opacity = i & enable_mask;
					uint8_t layer = pri_prom[pri_code * 0x20 + offset + opacity * 2];
					if (opacity) {
						if (opacity & top_mask) { if (layer != top) result |= 1; }
						else { if (layer == top) result |= 2; else result |= 4; }
					}
				}
				lo[offset] = ((lo[offset] << 4) | top) & 0xfffff;
				enable_mask &= ~top_mask;
				if (result & 1) { lo[offset] = 0xfffff; break; }
				if ((result & 6) == 6) { lo[offset] = 0xfffff; break; }
				if (result == 2) enable_mask = 0;
			} while (enable_mask);
		}
		uint32_t order = 0xfffff;
		for (int i = 5; i > 0;) {
			int layer;
			uint8_t l0 = lo[0] & 0x0f, l1 = lo[1] & 0x0f;
			if (l0 != 3) {
				if (l1 == 3) { layer = 4; lo[0] <<= 4; }
				else {
					layer = l0;
					if (l0 != l1) { order = 0xfffff; break; }
				}
			} else {
				if (l1 == 3) { layer = 0x43; order <<= 4; i--; }
				else { layer = 3; lo[1] <<= 4; }
			}
			order = (order << 4) | layer;
			i--;
			lo[0] >>= 4; lo[1] >>= 4;
		}
		layers_order[pri_code] = order & 0xfffff;
	}
}

void video_init(void)
{
	priority_create();
	memset(fb, 0, sizeof fb);
	for (int i = 0; i < 256 * 256; i++) spr_bm[i] = 0x7fff;
}

static inline uint32_t pal(int i)
{
	uint16_t d = g_m.palette[i & 0x3ff];
	int r = ((d >> 11) & 0x1e) | ((d >> 3) & 1);
	int g = ((d >> 7) & 0x1e) | ((d >> 2) & 1);
	int b = ((d >> 3) & 0x1e) | ((d >> 1) & 1);
	r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

/* returns 8-bit pen | (color << 4), or -1 if transparent */
static inline int layer_pixel(int L, int x, int ybm)
{
	const tmap_regs_t *t = &g_m.tmap[L];
	const uint16_t *ram = g_m.scrollram[L];
	int n = t->flag & 3;
	unsigned ti, tile;
	uint16_t code;
	int X, Y;
	if (!(t->flag & 0x10)) {            /* 16x16 tiles */
		static const int px[4] = { 16, 8, 4, 2 }, py[4] = { 2, 4, 8, 16 };
		int w = px[n] * 256, h = py[n] * 256, ncols = px[n] * 32;
		X = (x + t->scrollx) & (w - 1);
		Y = (ybm + t->scrolly) & (h - 1);
		int col = X >> 3, row = Y >> 3;
		ti = (((col / 2) * 16) + ((row / 2) / 16) * 256 * (ncols / 32) + ((row / 2) % 16)) * 4
		     + (row & 1) + (col & 1) * 2;
		code = ram[(ti >> 2) & 0x1fff];
		tile = (code & 0xfff) * 4 + (ti & 3);
	} else {                            /* 8x8 tiles */
		static const int px[4] = { 8, 4, 4, 2 }, py[4] = { 1, 2, 2, 4 };
		int w = px[n] * 256, h = py[n] * 256, ncols = px[n] * 32;
		X = (x + t->scrollx) & (w - 1);
		Y = (ybm + t->scrolly) & (h - 1);
		int col = X >> 3, row = Y >> 3;
		ti = col * 32 + (row / 32) * 1024 * (ncols / 32) + (row % 32);
		code = ram[ti & 0x1fff];
		tile = code & 0xfff;
	}
	tile %= g_roms.ntiles[L];
	int pen = g_roms.tiles[L][tile * 64 + (Y & 7) * 8 + (X & 7)];
	return pen | ((code >> 12) << 4);
}

static void draw_single_sprite(uint32_t code, uint32_t color, int sx, int sy,
                               int flipx, int flipy, int mosaic, int mossol, int pri)
{
	const uint8_t *gfx = g_roms.sprites + (size_t)(code % g_roms.nsprites) * 256;
	int yx = flipy ? 15 : 0, xx = flipx ? 15 : 0;
	color <<= 4;
	for (int y = 0; y < 16; y++, sy++) {
		if (sy < VIS_FIRST || sy > VIS_LAST) continue;
		uint16_t *dest = &spr_bm[sy * 256];
		int srcy = y ^ yx;
		const uint8_t *src = gfx + (mossol ? (srcy | mosaic) : (srcy & ~mosaic)) * 16;
		for (int x = 0, dx = sx; x < 16; x++, dx++) {
			if (dx < 0 || dx > 255) continue;
			int srcx = x ^ xx;
			uint8_t p = src[mossol ? (srcx | mosaic) : (srcx & ~mosaic)];
			if (p != 0x0f && !(dest[dx] & 0x8000))
				dest[dx] = (uint16_t)((p + color) | (pri << 14) | 0x8000);
		}
	}
}

static void draw_sprites(void)
{
	if (!(g_m.sprite_flag & 0x10)) {
		for (int i = VIS_FIRST * 256; i < (VIS_LAST + 1) * 256; i++) spr_bm[i] = 0x7fff;
	} else {
		int param = g_m.sprite_flag & 0x0f;
		for (int i = VIS_FIRST * 256; i < (VIS_LAST + 1) * 256; i++) {
			uint16_t p = spr_bm[i];
			spr_bm[i] = p & 0x7fff;
			if (((p & 0xf0) >> 4) < param) spr_bm[i] = 0x7fff;
		}
	}
	uint32_t color_mask = (g_m.sprite_flag & 0x100) ? 0x07 : 0x0f;
	const uint16_t *objectram = g_m.buf_obj[1];
	const uint16_t *spriteram = g_m.buf_spr[1];
	for (int offs = (0x800 - 8) / 2; offs >= 0; offs -= 4) {
		for (int sprite = 0; sprite < 4; sprite++) {
			const uint16_t *od = &objectram[offs + (0x800 / 2) * sprite];
			const uint16_t *sd = &spriteram[(od[0] & 0x7f) * 8];
			uint16_t attr = sd[4];
			if (((attr & 0xc0) >> 6) != sprite) continue;
			int sx = (sd[5] + od[1]) & 0x1ff; if (sx & 0x100) sx -= 0x200;
			int sy = (sd[6] + od[2]) & 0x1ff; if (sy & 0x100) sy -= 0x200;
			uint32_t code = sd[7] + od[3];
			int flipx = (attr >> 6) & 1, flipy = (attr >> 7) & 1;
			if (g_m.screen_flag & 1) { flipx = !flipx; flipy = !flipy; sx = 240 - sx; sy = 240 - sy; }
			code = (code & 0xfff) + ((g_m.sprite_bank & 1) << 12);
			draw_single_sprite(code, attr & color_mask, sx, sy, flipx, flipy,
			                   (attr & 0x0f00) >> 8, (attr >> 12) & 1, (attr >> 3) & 1);
		}
	}
}

void video_begin_frame(void)
{
	for (int i = 0; i < 0x400; i++) pal_rgb[i] = pal(i);
	draw_sprites();
}

void video_render_line(int vis)
{
	int ybm = vis + VIS_FIRST;
	uint32_t *out = &fb[vis * SCREEN_W];
	uint16_t line[256];
	uint8_t prio[256];
	uint32_t pri = layers_order[(g_m.active_layers & 0x0f00) >> 8];
	uint32_t reallyactive = 0, active;
	if (pri == 0xfffff) pri = 0x04132;
	for (int i = 0; i < 5; i++) reallyactive |= 1u << ((pri >> (4 * i)) & 0x0f);
	active = g_m.active_layers & reallyactive;
	active |= 1u << ((pri & 0xf0000) >> 16);

	/* palettes can change mid-frame (fades); refresh cheaply per line */
	for (int i = 0; i < 0x400; i++) pal_rgb[i] = pal(i);

	memset(prio, 0, sizeof prio);
	int opaque = 1;
	uint8_t primask = 0;
	for (int i = 0; i < 5; i++) {
		int layer = (pri & 0xf0000) >> 16;
		pri <<= 4;
		if (layer <= 2) {
			if (active & (1u << layer)) {
				for (int x = 0; x < 256; x++) {
					int p = layer_pixel(layer, x, ybm);
					if (opaque || (p & 15) != 15) {
						line[x] = (uint16_t)(p + layer * 256);
						prio[x] |= primask;
					}
				}
				opaque = 0;
			}
		} else if (layer == 3 || layer == 4) {
			if (opaque) { opaque = 0; memset(line, 0, sizeof line); }
			if (g_m.sprite_flag & 0x100) primask |= 1 << (layer - 3);
			else if (layer == 3) primask |= 3;
		}
	}
	if (opaque) memset(line, 0, sizeof line);
	if (active & 8) {
		const uint16_t *sl = &spr_bm[ybm * 256];
		for (int x = 0; x < 256; x++) {
			uint16_t p = sl[x];
			if ((p & 0xf) != 0xf) {
				uint32_t pr = (p & 0x4000) ? 0x0c : 0x0a;
				if (!((pr >> (prio[x] & 0x1f)) & 1))
					line[x] = (p & 0x3fff) + 0x300;
			}
		}
	}
	for (int x = 0; x < 256; x++) out[x] = pal_rgb[line[x] & 0x3ff];
}

void video_vblank(void)
{
	memcpy(g_m.buf_obj[1], g_m.buf_obj[0], sizeof g_m.buf_obj[0]);
	memcpy(g_m.buf_obj[0], g_m.objram, sizeof g_m.objram);
	memcpy(g_m.buf_spr[1], g_m.buf_spr[0], sizeof g_m.buf_spr[0]);
	memcpy(g_m.buf_spr[0], &g_m.ram[0x8000 / 2], 0x2000);
}
