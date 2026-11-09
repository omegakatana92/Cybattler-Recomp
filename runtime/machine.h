/* Cybattler (Jaleco 1993) - Mega System 1 type C machine model */
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_W 256
#define SCREEN_H 224
#define LINES_PER_FRAME 278
#define VIS_FIRST 16
#define VIS_LAST  239
#define MAIN_CLOCK  12000000
#define SND_CLOCK    7000000
#define PIXEL_CLOCK  6000000
#define HTOTAL 384
#define MAIN_CYC_PER_LINE (MAIN_CLOCK / (PIXEL_CLOCK / HTOTAL))   /* 768 */
#define SND_CYC_PER_LINE  (SND_CLOCK / (PIXEL_CLOCK / HTOTAL))    /* 448 */
#define FRAME_HZ ((double)PIXEL_CLOCK / (HTOTAL * LINES_PER_FRAME)) /* ~56.2 */

/* inputs, active-low bits as the game sees them */
enum {
	IN_RIGHT = 0x01, IN_LEFT = 0x02, IN_DOWN = 0x04, IN_UP = 0x08,
	IN_B1 = 0x10, IN_B2 = 0x20,
	SYS_START1 = 0x01, SYS_START2 = 0x02, SYS_SERVICE = 0x20,
	SYS_COIN1 = 0x40, SYS_COIN2 = 0x80
};

typedef struct {
	/* ROM (loaded once) */
	uint8_t main_rom[0x80000];
	uint8_t snd_rom[0x20000];
	uint8_t oki_rom[2][0x40000];
	uint8_t prom[0x200];
	uint8_t *tiles[3];        /* decoded 8x8 tiles, 1 byte/pixel */
	unsigned ntiles[3];
	uint8_t *sprites;         /* decoded 16x16 sprites, 1 byte/pixel */
	unsigned nsprites;
} roms_t;

typedef struct {
	uint16_t scrollx, scrolly, flag;
} tmap_regs_t;

typedef struct {
	uint8_t playing;
	uint32_t base, sample, count;
	int32_t signal, step;
	int32_t volume;
} oki_voice_t;

typedef struct {
	oki_voice_t v[4];
	int32_t command;   /* -1 = waiting for phrase byte */
} oki_t;

/* Everything below is plain data so save states are a memcpy. */
typedef struct {
	uint32_t version;
	uint16_t ram[0x8000];          /* 1c0000-1cffff (mirrored to 1fffff) */
	uint16_t objram[0x1000];       /* d2000-d3fff */
	uint16_t scrollram[3][0x2000]; /* e0000 / e8000 / f0000 */
	uint16_t palette[0x400];       /* f8000-f87ff */
	uint16_t snd_ram[0x8000];      /* sound cpu e0000-effff */
	tmap_regs_t tmap[3];
	uint16_t active_layers, sprite_flag, sprite_bank, screen_flag;
	uint16_t buf_obj[2][0x1000], buf_spr[2][0x1000];
	uint16_t latch_to_snd, latch_to_main;
	uint16_t ip_latched;
	uint8_t irq_main, irq_snd;     /* HOLD_LINE pending masks (bit n = level n) */
	uint8_t snd_in_reset;
	uint8_t in_sys, in_p1, in_p2, dsw1, dsw2;
	uint32_t frame;
	int line;
	/* sound chips (YM2151 core state is saved separately) */
	oki_t oki[2];
	int32_t ym_timer[2];           /* clocks remaining, -1 = off */
	uint32_t ym_frac;              /* clock remainder towards next sample */
} mstate_t;

extern roms_t g_roms;
extern mstate_t g_m;
extern int g_use_recomp;

int  machine_load_roms(const char *path);   /* zip or directory */
void machine_reset(void);
void machine_run_frame(void);               /* one 56.2 Hz frame */
const uint32_t *machine_framebuffer(void);  /* 256x224 XRGB8888, unrotated */
int  machine_audio(int16_t *out, int max_frames, int rate); /* pull stereo samples */

int  machine_save_state(const char *path);
int  machine_load_state(const char *path);

/* video.c */
void video_init(void);
void video_begin_frame(void);
void video_render_line(int vis_line);
void video_vblank(void);
uint32_t *video_fb(void);

/* sound */
void oki_reset(int chip);
void oki_write(int chip, uint8_t data);
void oki_render(int chip, float *buf, int n); /* n samples at 30303 Hz */

void ym_init(unsigned clock);
void ym_reset(void);
void ym_write(int port, uint8_t data);
uint8_t ym_read(void);
void ym_advance(int clocks, float *l, float *r, int *nsamples); /* generates output */
int  ym_irq(void);
int  ym_save(uint8_t *buf, int max);
int  ym_load(const uint8_t *buf, int len);

#ifdef __cplusplus
}
#endif
#endif
