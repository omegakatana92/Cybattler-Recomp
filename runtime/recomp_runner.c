/* Top-level executor for recompiled 68000 code, with interpreter fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_common.h"

extern void (*m68ki_instruction_jump_table[0x10000])(void);
extern int m68ki_initial_cycles;

#define MAXCPU 2
#define SPACE (1u << 24)
static unsigned char *g_miss[MAXCPU];
static unsigned g_miss_n[MAXCPU];
static unsigned char *g_trace[MAXCPU];
static int g_trace_on;

static void mark(unsigned char **bm, unsigned id, unsigned pc)
{
	if (!bm[id]) bm[id] = calloc(SPACE / 8, 1);
	pc &= SPACE - 1;
	bm[id][pc >> 3] |= 1u << (pc & 7);
}

void rc_trace_enable(int on) { g_trace_on = on; }

#ifdef RC_DEBUG
#include "machine.h"
void rc_debug_hook(unsigned pc)
{
	if (g_m.frame == 73 && g_m.line == 118) fprintf(stderr, "%06x\n", pc);
}
#endif
static inline void interp_one(int cpu_id)
{
#ifdef RC_DEBUG
	rc_debug_hook(REG_PC);
#endif
	if (g_trace_on) mark(g_trace, cpu_id, REG_PC);
	REG_PPC = REG_PC;
	REG_IR = m68ki_read_imm_16();
	m68ki_instruction_jump_table[REG_IR]();
	USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
}

int rc_execute(const rc_table *t, int num_cycles, int cpu_id)
{
	if (RESET_CYCLES) {
		int rc = RESET_CYCLES;
		RESET_CYCLES = 0;
		num_cycles -= rc;
		if (num_cycles <= 0) return rc;
	}
	SET_CYCLES(num_cycles);
	m68ki_initial_cycles = num_cycles;
	m68ki_check_interrupts();

	if (!CPU_STOPPED) {
		if (!t) {
			do interp_one(cpu_id); while (GET_CYCLES() > 0);
		} else {
			do {
				unsigned pc = REG_PC;
				unsigned c = pc >> t->chunk_bits;
				if (!(pc & 0xff000001) && c < t->nchunks && t->chunks[c] && t->chunks[c]())
					continue;
				/* not recompiled: interpret one instruction and remember it */
				if (!g_miss[cpu_id] || !(g_miss[cpu_id][(pc & (SPACE-1)) >> 3] & (1u << (pc & 7))))
					g_miss_n[cpu_id]++;
				mark(g_miss, cpu_id, pc);
				interp_one(cpu_id);
			} while (GET_CYCLES() > 0);
		}
		REG_PPC = REG_PC;
	} else
		SET_CYCLES(0);
	return m68ki_initial_cycles - GET_CYCLES();
}

unsigned rc_miss_count(int cpu_id) { return g_miss_n[cpu_id]; }

static void dump(unsigned char *bm, const char *path)
{
	FILE *f;
	if (!bm) return;
	f = fopen(path, "a");
	if (!f) return;
	for (unsigned a = 0; a < SPACE; a += 2)
		if (bm[a >> 3] & (1u << (a & 7))) fprintf(f, "%06x\n", a);
	fclose(f);
}

void rc_misses_dump(int cpu_id, const char *path) { dump(g_miss[cpu_id], path); }
void rc_trace_dump(int cpu_id, const char *path) { dump(g_trace[cpu_id], path); }
