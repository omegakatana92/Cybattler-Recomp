#!/usr/bin/env python3
"""Builds the 68000 opcode -> Musashi handler-name map used by recomp68k.py.
Compiles a small probe against the vendored Musashi, then writes handlermap.json."""
import re, subprocess, sys, os, json, tempfile
here=os.path.dirname(os.path.abspath(__file__))
mus=os.path.join(here,'..','third_party','musashi')
names=re.findall(r'^void (m68k_op_\w+)\(void\)', open(os.path.join(mus,'m68kops.c')).read(), re.M)
tmp=tempfile.mkdtemp()
c=os.path.join(tmp,'probe.c')
with open(c,'w') as f:
    f.write('#include <stdio.h>\n#include "m68k.h"\n#include "m68kcpu.h"\n')
    for n in names: f.write('void %s(void);\n'%n)
    f.write('static struct {const char*n; void(*f)(void);} T[]={\n')
    for n in names: f.write('{"%s",%s},\n'%(n,n))
    f.write('};\nunsigned int m68k_read_memory_8(unsigned int a){return 0;}\n'
            'unsigned int m68k_read_memory_16(unsigned int a){return 0;}\n'
            'unsigned int m68k_read_memory_32(unsigned int a){return 0;}\n'
            'void m68k_write_memory_8(unsigned int a,unsigned int v){}\n'
            'void m68k_write_memory_16(unsigned int a,unsigned int v){}\n'
            'void m68k_write_memory_32(unsigned int a,unsigned int v){}\n'
            'int mach_int_ack(int l){return M68K_INT_ACK_AUTOVECTOR;}\n'
            'extern unsigned char m68ki_cycles[][0x10000];\nextern void (*m68ki_instruction_jump_table[0x10000])(void);\n'
            'int main(){m68k_init();m68k_set_cpu_type(M68K_CPU_TYPE_68000);\n'
            'int n=sizeof(T)/sizeof(T[0]);\n'
            'for(int op=0;op<65536;op++){void(*h)(void)=m68ki_instruction_jump_table[op];const char*nm="?";\n'
            ' for(int i=0;i<n;i++) if(T[i].f==h){nm=T[i].n;break;}\n'
            ' printf("%s %d\\n",nm,m68ki_cycles[0][op]);}\nreturn 0;}\n')
exe=os.path.join(tmp,'probe')
srcs=[c]+[os.path.join(mus,x) for x in ('m68kcpu.c','m68kops.c','softfloat/softfloat.c')]
subprocess.check_call(['gcc','-O1','-w','-I',mus,'-o',exe]+srcs+['-lm'])
out=subprocess.check_output([exe]).decode().split('\n')
hm=[]; cyc=[]
for l in out[:65536]:
    n,cy=l.split(); hm.append(n); cyc.append(int(cy))
json.dump({'handler':hm,'cycles':cyc}, open(os.path.join(here,'handlermap.json'),'w'))
print('mapped', sum(1 for h in hm if h!='m68k_op_illegal'), 'legal opcodes;', len(set(hm)), 'distinct handlers')
