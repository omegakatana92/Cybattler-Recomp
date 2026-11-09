/* ctypes helper: exposes Musashi's disassembler over an in-memory ROM image */
#include "m68k.h"
static const unsigned char *g_img; static unsigned int g_size;
void dh_set_image(const unsigned char *img, unsigned int size){ g_img=img; g_size=size; }
static unsigned int rd8(unsigned int a){ return a<g_size? g_img[a]:0xff; }
unsigned int m68k_read_disassembler_8(unsigned int a){ return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return (rd8(a)<<8)|rd8(a+1); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return (m68k_read_disassembler_16(a)<<16)|m68k_read_disassembler_16(a+2); }
unsigned int m68k_read_memory_8(unsigned int a){ return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a){ return m68k_read_disassembler_16(a); }
unsigned int m68k_read_memory_32(unsigned int a){ return m68k_read_disassembler_32(a); }
unsigned int dh_disasm(char *buf, unsigned int pc){ return m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000); }
unsigned int dh_valid(unsigned int op){ return m68k_is_valid_instruction(op, M68K_CPU_TYPE_68000); }
