/* OKI MSM6295 ADPCM, 4 voices. 4 MHz clock, pin 7 high -> 30303 Hz. */
#include <string.h>
#include "machine.h"

static const int8_t idx_shift[16] = { -1,-1,-1,-1, 2,4,6,8, -1,-1,-1,-1, 2,4,6,8 };
static int diff_lookup[49 * 16];
static const int vol_table[16] = { 0x20,0x16,0x10,0x0b,0x08,0x06,0x04,0x03,0x02,0x01,0,0,0,0,0,0 };
static int tables_ready;

static void init_tables(void)
{
	static const int nbl2bit[16][4] = {
		{ 1,0,0,0},{ 1,0,0,1},{ 1,0,1,0},{ 1,0,1,1},{ 1,1,0,0},{ 1,1,0,1},{ 1,1,1,0},{ 1,1,1,1},
		{-1,0,0,0},{-1,0,0,1},{-1,0,1,0},{-1,0,1,1},{-1,1,0,0},{-1,1,0,1},{-1,1,1,0},{-1,1,1,1}};
	for (int step = 0; step <= 48; step++) {
		int stepval = (int)(16.0 * __builtin_pow(11.0 / 10.0, (double)step));
		for (int n = 0; n < 16; n++)
			diff_lookup[step * 16 + n] = nbl2bit[n][0] *
				(stepval * nbl2bit[n][1] + stepval / 2 * nbl2bit[n][2] +
				 stepval / 4 * nbl2bit[n][3] + stepval / 8);
	}
	tables_ready = 1;
}

void oki_reset(int chip)
{
	if (!tables_ready) init_tables();
	memset(&g_m.oki[chip], 0, sizeof(oki_t));
	g_m.oki[chip].command = -1;
}

static inline int clock_adpcm(oki_voice_t *v, int nibble)
{
	v->signal += diff_lookup[v->step * 16 + (nibble & 15)];
	if (v->signal > 2047) v->signal = 2047;
	else if (v->signal < -2048) v->signal = -2048;
	v->step += idx_shift[nibble & 7];
	if (v->step > 48) v->step = 48;
	else if (v->step < 0) v->step = 0;
	return v->signal;
}

void oki_write(int chip, uint8_t data)
{
	oki_t *o = &g_m.oki[chip];
	const uint8_t *rom = g_roms.oki_rom[chip];
	if (o->command != -1) {
		int voicemask = data >> 4;
		for (int i = 0; i < 4; i++) {
			if (!(voicemask & (8 >> i))) continue;
			oki_voice_t *v = &o->v[i];
			if (!v->playing) {
				uint32_t base = o->command * 8;
				uint32_t start = ((rom[base] << 16) | (rom[base+1] << 8) | rom[base+2]) & 0x3ffff;
				uint32_t stop  = ((rom[base+3] << 16) | (rom[base+4] << 8) | rom[base+5]) & 0x3ffff;
				if (start < stop) {
					v->playing = 1;
					v->base = start;
					v->sample = 0;
					v->count = 2 * (stop - start + 1);
					v->signal = -2; v->step = 0;
					v->volume = vol_table[data & 0x0f];
				} else
					v->playing = 0;
			}
		}
		o->command = -1;
	} else if (data & 0x80) {
		o->command = data & 0x7f;
	} else {
		int voicemask = data >> 3;
		for (int i = 0; i < 4; i++)
			if (voicemask & (8 >> i)) o->v[i].playing = 0;
	}
}

void oki_render(int chip, float *buf, int n)
{
	oki_t *o = &g_m.oki[chip];
	const uint8_t *rom = g_roms.oki_rom[chip];
	for (int s = 0; s < n; s++) buf[s] = 0;
	for (int i = 0; i < 4; i++) {
		oki_voice_t *v = &o->v[i];
		if (!v->playing) continue;
		for (int s = 0; s < n; s++) {
			int b = rom[(v->base + v->sample / 2) & 0x3ffff];
			int nib = (v->sample & 1) ? (b & 15) : (b >> 4);
			int sig = clock_adpcm(v, nib);
			buf[s] += (float)(sig * v->volume / 2) / 2048.0f / 16.0f;
			if (++v->sample >= v->count) { v->playing = 0; break; }
		}
	}
}
