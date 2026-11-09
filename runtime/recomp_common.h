/* Shared by generated code and the runner. */
#ifndef RECOMP_COMMON_H
#define RECOMP_COMMON_H

#include "m68k.h"
#include "m68kcpu.h"

typedef struct {
	int (*const *chunks)(void);
	unsigned nchunks;
	unsigned chunk_bits;
	unsigned ninsn;
} rc_table;

/* One recompiled 68000 instruction: identical state transitions to one
   iteration of Musashi's m68k_execute() loop, minus fetch/decode. */
#ifdef RC_DEBUG
void rc_debug_hook(unsigned pc);
#define RC_HOOK(pc) rc_debug_hook(pc)
#else
#define RC_HOOK(pc)
#endif
#define RC_STEP(pc, op, pc2, fn, cyc) do { RC_HOOK(pc); \
	REG_PPC = (pc); REG_IR = (op); REG_PC = (pc2); \
	fn(); USE_CYCLES(cyc); } while (0)

/* Runs `cycles` cycles on the current Musashi context using the
   recompiled table (or the interpreter if table == NULL). */
int rc_execute(const rc_table *t, int cycles, int cpu_id);

/* Coverage bookkeeping for PCs that had to be interpreted */
void rc_misses_dump(int cpu_id, const char *path);
unsigned rc_miss_count(int cpu_id);
void rc_trace_enable(int on);
void rc_trace_dump(int cpu_id, const char *path);

extern const rc_table rc_main_table;
extern const rc_table rc_snd_table;

#endif
