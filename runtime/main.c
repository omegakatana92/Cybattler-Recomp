/* Cybattler native port - SDL2 frontend */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "machine.h"
#include "recomp_common.h"

#define AUDIO_RATE 48000

static void usage(void)
{
	printf("cybattler [options] [path/to/cybattlr.zip | romdir]\n"
	       "  --interp           run on the reference interpreter instead of recompiled code\n"
	       "  --fullscreen       start fullscreen (default on Steam Deck)\n"
	       "  --window SCALE     windowed at SCALE x\n"
	       "  --headless N       run N frames without a window (testing)\n"
	       "  --dump DIR         with --headless: write every 60th frame as BMP\n"
	       "  --hash FILE        write a per-frame state hash (for verification)\n"
	       "  --trace            record executed PCs (seeds for the recompiler)\n"
	       "  --script FILE      scripted inputs: lines 'frame sys p1' (hex)\n"
	       "  --dsw1 HEX --dsw2 HEX  dip switches (default ff / bf)\n");
}

static uint32_t fnv(const void *p, size_t n, uint32_t h)
{
	const uint8_t *b = p;
	while (n--) { h ^= *b++; h *= 16777619u; }
	return h;
}

static void write_bmp_rot(const char *path, const uint32_t *fb)
{
	/* rotate 90 degrees clockwise: output is 224 wide, 256 tall */
	int W = SCREEN_H, H = SCREEN_W;
	FILE *f = fopen(path, "wb");
	if (!f) return;
	int rowbytes = W * 3, pad = (4 - rowbytes % 4) % 4, datasz = (rowbytes + pad) * H;
	uint8_t hdr[54] = { 'B', 'M' };
	*(uint32_t *)(hdr + 2) = 54 + datasz; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
	*(int32_t *)(hdr + 18) = W; *(int32_t *)(hdr + 22) = H; *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24;
	*(uint32_t *)(hdr + 34) = datasz;
	fwrite(hdr, 1, 54, f);
	for (int y = H - 1; y >= 0; y--) {
		for (int x = 0; x < W; x++) {
			/* rotated(x,y) = src(sx = y, sy = SCREEN_H-1-x) */
			uint32_t c = fb[(SCREEN_H - 1 - x) * SCREEN_W + y];
			uint8_t px[3] = { c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff };
			fwrite(px, 1, 3, f);
		}
		fwrite("\0\0\0", 1, pad, f);
	}
	fclose(f);
}

typedef struct { uint32_t frame; uint8_t sys, p1; } script_ev;
static script_ev *g_script; static int g_nscript;

static void load_script(const char *path)
{
	FILE *f = fopen(path, "r");
	char l[256];
	if (!f) { fprintf(stderr, "can't open script %s\n", path); return; }
	while (fgets(l, sizeof l, f)) {
		unsigned fr, s, p;
		if (sscanf(l, "%u %x %x", &fr, &s, &p) == 3) {
			g_script = realloc(g_script, sizeof *g_script * (g_nscript + 1));
			g_script[g_nscript++] = (script_ev){ fr, (uint8_t)s, (uint8_t)p };
		}
	}
	fclose(f);
}

static void apply_script(uint32_t frame)
{
	for (int i = 0; i < g_nscript; i++)
		if (g_script[i].frame == frame) { g_m.in_sys = g_script[i].sys; g_m.in_p1 = g_script[i].p1; }
}

static const char *find_rom(const char *arg)
{
	static char buf[1024];
	const char *base = SDL_GetBasePath();
	const char *cands[] = { "cybattlr.zip", "roms/cybattlr.zip", "cybattlr", NULL };
	FILE *f;
	if (arg) return arg;
	for (int i = 0; cands[i]; i++) {
		if (base) { snprintf(buf, sizeof buf, "%s%s", base, cands[i]); if ((f = fopen(buf, "rb"))) { fclose(f); return buf; } }
		if ((f = fopen(cands[i], "rb"))) { fclose(f); return cands[i]; }
	}
	/* next to the AppImage file (SDL_GetBasePath points inside the mounted image) */
	const char *ai = getenv("APPIMAGE");
	if (ai) {
		char d[1024]; snprintf(d, sizeof d, "%s", ai);
		char *sl = strrchr(d, '/'); if (sl) *sl = 0;
		snprintf(buf, sizeof buf, "%s/cybattlr.zip", d);
		if ((f = fopen(buf, "rb"))) { fclose(f); return buf; }
		snprintf(buf, sizeof buf, "%s/roms/cybattlr.zip", d);
		if ((f = fopen(buf, "rb"))) { fclose(f); return buf; }
	}
	{
		char *pref = SDL_GetPrefPath("recomp", "cybattler");
		if (pref) {
			snprintf(buf, sizeof buf, "%scybattlr.zip", pref); SDL_free(pref);
			if ((f = fopen(buf, "rb"))) { fclose(f); return buf; }
		}
	}
	const char *home = getenv("HOME");
	if (!home) home = getenv("USERPROFILE");
	if (home) {
		snprintf(buf, sizeof buf, "%s/Downloads/cybattlr.zip", home);
		if ((f = fopen(buf, "rb"))) { fclose(f); return buf; }
	}
	return "cybattlr.zip";
}

static void state_path(char *out, size_t n)
{
	char *pref = SDL_GetPrefPath("recomp", "cybattler");
	snprintf(out, n, "%sstate1.sav", pref ? pref : "");
	SDL_free(pref);
}

int main(int argc, char **argv)
{
	const char *rom = NULL, *dumpdir = NULL, *hashfile = NULL, *script = NULL;
	int headless = 0, trace = 0, fullscreen = -1, scale = 3;
	unsigned dsw1 = 0xff, dsw2 = 0xbf;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--interp")) g_use_recomp = 0;
		else if (!strcmp(argv[i], "--headless") && i + 1 < argc) headless = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dumpdir = argv[++i];
		else if (!strcmp(argv[i], "--hash") && i + 1 < argc) hashfile = argv[++i];
		else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
		else if (!strcmp(argv[i], "--trace")) trace = 1;
		else if (!strcmp(argv[i], "--test-savestate")) headless = -1;
		else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
		else if (!strcmp(argv[i], "--window") && i + 1 < argc) { fullscreen = 0; scale = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--dsw1") && i + 1 < argc) dsw1 = strtoul(argv[++i], 0, 16);
		else if (!strcmp(argv[i], "--dsw2") && i + 1 < argc) dsw2 = strtoul(argv[++i], 0, 16);
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
		else rom = argv[i];
	}
	if (trace) { g_use_recomp = 0; rc_trace_enable(1); }
	{ extern FILE *g_dbg; if (getenv("CYB_DBG")) g_dbg = fopen(getenv("CYB_DBG"), "w"); }
	if (SDL_Init(headless ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER)) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
	}
	rom = find_rom(rom);
	if (!machine_load_roms(rom)) {
		char msg[1200];
		snprintf(msg, sizeof msg, "Could not load the Cybattler ROM set from:\n%s\n\n"
		         "Put cybattlr.zip next to the program or pass its path.", rom);
		fprintf(stderr, "%s\n", msg);
		if (!headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cybattler", msg, NULL);
		return 1;
	}
	g_m.dsw1 = (uint8_t)dsw1; g_m.dsw2 = (uint8_t)dsw2;
	machine_reset();
	g_m.dsw1 = (uint8_t)dsw1; g_m.dsw2 = (uint8_t)dsw2;
	printf("Cybattler: %s mode, ROM %s\n", g_use_recomp ? "recompiled" : "interpreter", rom);
	if (script) load_script(script);

	if (headless == -1) {   /* save, run, load, re-run: hashes must match */
		uint32_t h1 = 0, h2 = 0;
		for (int f = 0; f < 2000; f++) machine_run_frame();
		if (!machine_save_state("st_test.sav")) { puts("save failed"); return 1; }
		for (int f = 0; f < 600; f++) { machine_run_frame(); h1 = fnv(g_m.ram, sizeof g_m.ram, h1 ^ fnv(machine_framebuffer(), SCREEN_W * SCREEN_H * 4, 0)); }
		if (!machine_load_state("st_test.sav")) { puts("load failed"); return 1; }
		for (int f = 0; f < 600; f++) { machine_run_frame(); h2 = fnv(g_m.ram, sizeof g_m.ram, h2 ^ fnv(machine_framebuffer(), SCREEN_W * SCREEN_H * 4, 0)); }
		printf("savestate %s (%08x / %08x)\n", h1 == h2 ? "OK" : "MISMATCH", h1, h2);
		return h1 != h2;
	}
	if (headless) {
		FILE *hf = hashfile ? fopen(hashfile, "w") : NULL;
		static int16_t abuf[4096 * 2];
		double peak = 0;
		FILE *wav = getenv("CYB_WAV") ? fopen(getenv("CYB_WAV"), "wb") : NULL;
		if (wav) fseek(wav, 44, SEEK_SET);
		Uint64 t0 = SDL_GetPerformanceCounter();
		for (int f = 0; f < headless; f++) {
			apply_script(g_m.frame);
			machine_run_frame();
			int n = machine_audio(abuf, 4096, AUDIO_RATE);
			for (int i = 0; i < n * 2; i++) if (abs(abuf[i]) > peak) peak = abs(abuf[i]);
			if (wav) fwrite(abuf, 4, n, wav);
			if (hf) {
				uint32_t h = fnv(g_m.ram, sizeof g_m.ram, 2166136261u);
				h = fnv(g_m.snd_ram, sizeof g_m.snd_ram, h);
				h = fnv(machine_framebuffer(), SCREEN_W * SCREEN_H * 4, h);
				fprintf(hf, "%d %08x\n", f, h);
			}
			if (dumpdir && (f % 60 == 59 || f == headless - 1)) {
				char p[1024]; snprintf(p, sizeof p, "%s/frame_%05d.bmp", dumpdir, f + 1);
				write_bmp_rot(p, machine_framebuffer());
			}
		}
		double secs = (double)(SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
		printf("%d frames in %.2fs (%.1f fps, %.1fx realtime); audio peak %.0f\n",
		       headless, secs, headless / secs, headless / secs / FRAME_HZ, peak);
		printf("interpreted fallback PCs: main %u, sound %u\n", rc_miss_count(0), rc_miss_count(1));
		if (hf) fclose(hf);
		if (wav) {
			uint32_t len = (uint32_t)ftell(wav) - 44, v;
			uint8_t h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0 };
			v = len + 36; memcpy(h + 4, &v, 4); v = AUDIO_RATE; memcpy(h + 24, &v, 4);
			v = AUDIO_RATE * 4; memcpy(h + 28, &v, 4); h[32] = 4; h[34] = 16;
			memcpy(h + 36, "data", 4); memcpy(h + 40, &len, 4);
			fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
		}
		if (trace) { rc_trace_dump(0, "trace_main.txt"); rc_trace_dump(1, "trace_snd.txt"); }
		else { rc_misses_dump(0, "missing_main.txt"); rc_misses_dump(1, "missing_snd.txt"); }
		return 0;
	}

	/* ---------------- interactive ---------------- */
	if (fullscreen < 0) {
		SDL_DisplayMode dm;
		fullscreen = (getenv("SteamDeck") || getenv("SteamAppId") ||
		              (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w == 1280 && dm.h == 800)) ? 1 : 0;
	}
	SDL_Window *win = SDL_CreateWindow("Cybattler", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
	                                   SCREEN_H * scale, SCREEN_W * scale,
	                                   SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
	SDL_Texture *texr = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_H, SCREEN_W);
	static uint32_t rot[SCREEN_W * SCREEN_H];
	SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);

	SDL_AudioSpec want = { 0 }, have;
	want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
	SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (adev) SDL_PauseAudioDevice(adev, 0);
	SDL_GameController *pads[4] = { 0 };

	int running = 1, paused = 0, tate = 0, smooth = 0;
	Uint64 freq = SDL_GetPerformanceFrequency(), next = SDL_GetPerformanceCounter();
	double frame_ticks = freq / FRAME_HZ;
	static int16_t abuf[4096 * 2];
	char spath[1024]; state_path(spath, sizeof spath);

	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) running = 0;
			else if (e.type == SDL_CONTROLLERDEVICEADDED) {
				for (int i = 0; i < 4; i++) if (!pads[i]) { pads[i] = SDL_GameControllerOpen(e.cdevice.which); break; }
			} else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
				switch (e.key.keysym.sym) {
				case SDLK_ESCAPE: running = 0; break;
				case SDLK_F3: machine_reset(); g_m.dsw1 = (uint8_t)dsw1; g_m.dsw2 = (uint8_t)dsw2; break;
				case SDLK_F5: machine_save_state(spath); break;
				case SDLK_F7: machine_load_state(spath); break;
				case SDLK_F11: case SDLK_f:
					fullscreen = !fullscreen;
					SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
					SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE); break;
				case SDLK_t: tate = !tate; break;
				case SDLK_s: smooth = !smooth;
					SDL_SetTextureScaleMode(tex, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
					SDL_SetTextureScaleMode(texr, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
					break;
				case SDLK_p: paused = !paused; break;
				}
			}
		}
		/* inputs */
		const Uint8 *k = SDL_GetKeyboardState(NULL);
		uint8_t p1 = 0, p2 = 0, sys = 0;
		if (k[SDL_SCANCODE_RIGHT]) p1 |= IN_RIGHT;
		if (k[SDL_SCANCODE_LEFT]) p1 |= IN_LEFT;
		if (k[SDL_SCANCODE_DOWN]) p1 |= IN_DOWN;
		if (k[SDL_SCANCODE_UP]) p1 |= IN_UP;
		if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_LCTRL]) p1 |= IN_B1;
		if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_LALT]) p1 |= IN_B2;
		if (k[SDL_SCANCODE_1] || k[SDL_SCANCODE_RETURN]) sys |= SYS_START1;
		if (k[SDL_SCANCODE_2]) sys |= SYS_START2;
		if (k[SDL_SCANCODE_5]) sys |= SYS_COIN1;
		if (k[SDL_SCANCODE_6]) sys |= SYS_COIN2;
		if (k[SDL_SCANCODE_9]) sys |= SYS_SERVICE;
		if (k[SDL_SCANCODE_L]) p2 |= IN_RIGHT;
		if (k[SDL_SCANCODE_J]) p2 |= IN_LEFT;
		if (k[SDL_SCANCODE_K]) p2 |= IN_DOWN;
		if (k[SDL_SCANCODE_I]) p2 |= IN_UP;
		if (k[SDL_SCANCODE_Q]) p2 |= IN_B1;
		if (k[SDL_SCANCODE_W]) p2 |= IN_B2;
		for (int i = 0; i < 4; i++) {
			SDL_GameController *c = pads[i];
			if (!c) continue;
			uint8_t *pp = i == 0 ? &p1 : &p2;
			int ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
			int ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || ax > 12000) *pp |= IN_RIGHT;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || ax < -12000) *pp |= IN_LEFT;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || ay > 12000) *pp |= IN_DOWN;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) || ay < -12000) *pp |= IN_UP;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ||
			    SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X)) *pp |= IN_B1;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ||
			    SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) *pp |= IN_B2;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) sys |= i == 0 ? SYS_START1 : SYS_START2;
			if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) sys |= i == 0 ? SYS_COIN1 : SYS_COIN2;
			if (i == 0 && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) &&
			    SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
				if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START)) running = 0; /* L+R+Start = quit */
			}
		}
		g_m.in_p1 = (uint8_t)~p1; g_m.in_p2 = (uint8_t)~p2; g_m.in_sys = (uint8_t)~sys;

		if (!paused) {
			machine_run_frame();
			int n = machine_audio(abuf, 4096, have.freq ? have.freq : AUDIO_RATE);
			if (adev && SDL_GetQueuedAudioSize(adev) < (Uint32)(have.freq * 4 / 8)) /* < 125 ms */
				SDL_QueueAudio(adev, abuf, n * 4);
		}

		/* present: ROT90 game. Rotate in software (cheap, avoids RenderCopyEx
		   quirks on some drivers) unless TATE mode is on. */
		const uint32_t *fbp = machine_framebuffer();
		int ww, wh; SDL_GetRendererOutputSize(ren, &ww, &wh);
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_RenderClear(ren);
		SDL_Texture *t = tate ? tex : texr;
		if (tate) SDL_UpdateTexture(tex, NULL, fbp, SCREEN_W * 4);
		else {
			for (int y = 0; y < SCREEN_W; y++)        /* rotated is 224 wide, 256 tall */
				for (int x = 0; x < SCREEN_H; x++)
					rot[y * SCREEN_H + x] = fbp[(SCREEN_H - 1 - x) * SCREEN_W + y];
			SDL_UpdateTexture(texr, NULL, rot, SCREEN_H * 4);
		}
		double gw = tate ? SCREEN_W : SCREEN_H, gh = tate ? SCREEN_H : SCREEN_W;
		double sc = (double)ww / gw < (double)wh / gh ? (double)ww / gw : (double)wh / gh;
		int dw = (int)(gw * sc), dh = (int)(gh * sc);
		SDL_Rect d = { (ww - dw) / 2, (wh - dh) / 2, dw, dh };
		SDL_RenderCopy(ren, t, NULL, &d);
		SDL_RenderPresent(ren);

		/* pace to 56.2 Hz (vsync may already have waited) */
		next += (Uint64)frame_ticks;
		Uint64 now = SDL_GetPerformanceCounter();
		if (now < next) {
			Uint32 ms = (Uint32)((next - now) * 1000 / freq);
			if (ms > 1) SDL_Delay(ms - 1);
			while (SDL_GetPerformanceCounter() < next) {}
		} else if (now - next > freq / 4) next = now;
	}
	if (adev) SDL_CloseAudioDevice(adev);
	SDL_Quit();
	return 0;
}
