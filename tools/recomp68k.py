#!/usr/bin/env python3
"""
recomp68k.py - static recompiler for 68000 program ROMs -> C.

How it works
  1. Recursive-descent disassembly starting from the exception vectors plus any
     extra seed addresses (seed files, runtime traces). Control flow is followed
     through Bcc/BRA/BSR/DBcc, JMP/JSR with static effective addresses, TRAPs.
  2. Every discovered instruction becomes a labelled C statement sequence that
     calls the exact Musashi opcode handler for that opcode word directly (no
     fetch, no decode, no jump-table lookup), charges its base cycle cost, and
     then branches with a static `goto` when the target is known at compile
     time. Indirect transfers (RTS, RTE, JMP (An), jump tables) go through a
     per-chunk `switch` on the program counter.
  3. Code is split into 4 KiB chunks, one C function each. A top-level runner
     (runtime/recomp_runner.c) picks the chunk for the current PC; addresses
     that were never discovered fall back to the interpreter and are logged so
     they can be fed back in as seeds.

Semantics are identical to the Musashi interpreter instruction-for-instruction
(including cycle accounting), which is what makes lock-step verification
against the interpreter possible (see runtime/main.c --verify).
"""
import argparse, ctypes, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Handlers that can raise an exception or change SR/PC in non-obvious ways.
# After these we compare REG_PC to the expected fall-through address.
MAYTRAP_KEYS = ('divs', 'divu', 'chk', 'trapv', 'illegal', '1010', '1111',
                'move_16_tos', 'andi_16_tos', 'ori_16_tos', 'eori_16_tos',
                'stop', 'reset', 'move_32_tou', 'move_32_fru', 'trap',
                'andi_16_tos', 'move_16_frs')


class Dasm:
    def __init__(self, img):
        lib = os.path.join(HERE, 'libdasm.so')
        self.lib = ctypes.CDLL(lib)
        self.buf = ctypes.create_string_buffer(bytes(img))
        self.lib.dh_set_image(self.buf, len(img))
        self.out = ctypes.create_string_buffer(256)

    def valid(self, op):
        return bool(self.lib.dh_valid(op))

    def __call__(self, pc):
        n = self.lib.dh_disasm(self.out, pc)
        return n, self.out.value.decode('latin-1')


def sext8(v): return v - 0x100 if v & 0x80 else v
def sext16(v): return v - 0x10000 if v & 0x8000 else v


def analyse(img, rom_end, seeds, hmap, dasm, verbose=False):
    w16 = lambda a: struct.unpack_from('>H', img, a)[0]
    w32 = lambda a: struct.unpack_from('>I', img, a)[0]
    insns = {}      # addr -> dict
    work = [s for s in seeds if 0 <= s < rom_end and not s & 1]
    while work:
        pc = work.pop()
        while True:
            if pc in insns or pc >= rom_end or pc < 0 or pc & 1:
                break
            op = w16(pc)
            h = hmap['handler'][op]
            size, text = dasm(pc)
            if size <= 0 or pc + size > rom_end:
                break
            ins = {'pc': pc, 'op': op, 'h': h, 'size': size, 'text': text,
                   'cyc': hmap['cycles'][op], 'targets': [], 'fall': True,
                   'kind': 'normal'}
            nxt = pc + size
            hi = op >> 12
            if (h in ('m68k_op_illegal', 'm68k_op_1010', 'm68k_op_1111')
                    or hi in (0xa, 0xf) or not dasm.valid(op)):
                # not a 68000 instruction: raises an exception (line A/F,
                # illegal). Cybattler's task kernel uses line-F as a syscall.
                ins['kind'] = 'exc'; ins['fall'] = False; ins['size'] = 2
                ins['text'] = 'dc.w $%04x ; exception' % op
            elif hi == 6:                                   # Bcc / BRA / BSR
                d = op & 0xff
                disp = sext16(w16(pc + 2)) if d == 0 else sext8(d)
                tgt = (pc + 2 + disp) & 0xffffffff
                cond = (op >> 8) & 0xf
                ins['targets'] = [tgt]
                ins['kind'] = 'cf'
                if cond == 0:
                    ins['fall'] = False                       # BRA
            elif (op & 0xf0f8) == 0x50c8:                       # DBcc
                tgt = (pc + 2 + sext16(w16(pc + 2))) & 0xffffffff
                ins['targets'] = [tgt]; ins['kind'] = 'cf'
            elif (op & 0xffc0) in (0x4ec0, 0x4e80):            # JMP / JSR
                mode, reg = (op >> 3) & 7, op & 7
                tgt = None
                if mode == 7 and reg == 0: tgt = sext16(w16(pc + 2)) & 0xffffff
                elif mode == 7 and reg == 1: tgt = w32(pc + 2) & 0xffffff
                elif mode == 7 and reg == 2: tgt = (pc + 2 + sext16(w16(pc + 2))) & 0xffffff
                if tgt is not None: ins['targets'] = [tgt]
                ins['kind'] = 'cf'
                if (op & 0xffc0) == 0x4ec0: ins['fall'] = False
            elif op in (0x4e75, 0x4e73, 0x4e77):               # RTS RTE RTR
                ins['kind'] = 'cf'; ins['fall'] = False
            elif (op & 0xfff0) == 0x4e40:                     # TRAP #n
                vec = 32 + (op & 15)
                ins['targets'] = [w32(vec * 4) & 0xffffff]
                ins['kind'] = 'cf'
            elif op == 0x4e72:                                # STOP
                ins['kind'] = 'cf'
            elif any(k in h for k in MAYTRAP_KEYS):
                ins['kind'] = 'maytrap'
            insns[pc] = ins
            for t in ins['targets']:
                if t < rom_end and t not in insns:
                    work.append(t)
            if not ins['fall']:
                break
            pc = nxt
    return insns


def emit(insns, prefix, out, chunk_bits=12):
    chunks = {}
    for pc in sorted(insns):
        chunks.setdefault(pc >> chunk_bits, []).append(insns[pc])
    o = []
    o.append('/* GENERATED by tools/recomp68k.py - do not edit */')
    o.append('#include "recomp_common.h"\n')
    hs = sorted({i['h'] for i in insns.values()})
    for h in hs:
        o.append('void %s(void);' % h)
    o.append('')
    for cid, lst in sorted(chunks.items()):
        addrs = {i['pc'] for i in lst}
        o.append('static int %s_c%04x(void)\n{' % (prefix, cid))
        o.append('\tswitch (REG_PC) {')
        for i in lst:
            o.append('\tcase 0x%06x: goto L_%06x;' % (i['pc'], i['pc']))
        o.append('\tdefault: return 0;\n\t}')
        o.append('dispatch:\n\tswitch (REG_PC) {')
        for i in lst:
            o.append('\tcase 0x%06x: goto L_%06x;' % (i['pc'], i['pc']))
        o.append('\tdefault: return 1;\n\t}')
        for idx, i in enumerate(lst):
            pc, nxt = i['pc'], i['pc'] + i['size']
            o.append('L_%06x: /* %s */' % (pc, i['text']))
            o.append('\tRC_STEP(0x%06x, 0x%04x, 0x%06x, %s, %d);'
                     % (pc, i['op'], pc + 2, i['h'], i['cyc']))
            o.append('\tif (GET_CYCLES() <= 0) return 1;')
            follow = nxt in addrs
            if i['kind'] == 'normal':
                if not follow:
                    o.append('\treturn 1;')
                # else fall through to next label
                elif idx + 1 < len(lst) and lst[idx + 1]['pc'] != nxt:
                    o.append('\tgoto L_%06x;' % nxt)
            elif i['kind'] == 'maytrap':
                if follow:
                    o.append('\tif (REG_PC == 0x%06x) goto L_%06x;' % (nxt, nxt))
                o.append('\tgoto dispatch;')
            elif i['kind'] == 'cf':
                for t in i['targets']:
                    if t in addrs:
                        o.append('\tif (REG_PC == 0x%06x) goto L_%06x;' % (t, t))
                if i['fall'] and follow:
                    o.append('\tif (REG_PC == 0x%06x) goto L_%06x;' % (nxt, nxt))
                o.append('\tgoto dispatch;')
            else:  # exc
                o.append('\tgoto dispatch;')
        o.append('}\n')
    nchunks = (max(chunks) + 1) if chunks else 1
    o.append('static int (*const %s_chunks[%d])(void) = {' % (prefix, nchunks))
    for c in range(nchunks):
        o.append('\t%s,' % ('%s_c%04x' % (prefix, c) if c in chunks else '0'))
    o.append('};\n')
    o.append('const rc_table %s_table = { %s_chunks, %d, %d, %d };'
             % (prefix, prefix, nchunks, chunk_bits, len(insns)))
    open(out, 'w').write('\n'.join(o) + '\n')
    return len(chunks)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom', help='big-endian program image')
    ap.add_argument('--prefix', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rom-end', type=lambda x: int(x, 0))
    ap.add_argument('--seeds', nargs='*', default=[], help='files with hex addresses')
    ap.add_argument('--vectors', type=int, default=48)
    ap.add_argument('--listing', help='write a disassembly listing here')
    a = ap.parse_args()
    img = open(a.rom, 'rb').read()
    rom_end = a.rom_end or len(img)
    hmap = json.load(open(os.path.join(HERE, 'handlermap.json')))
    dasm = Dasm(img)
    seeds = set()
    for v in range(1, a.vectors):
        t = struct.unpack_from('>I', img, v * 4)[0] & 0xffffff
        if t < rom_end and not t & 1:
            seeds.add(t)
    for f in a.seeds:
        if os.path.exists(f):
            for l in open(f):
                l = l.split('#')[0].strip()
                if l:
                    seeds.add(int(l, 16))
    insns = analyse(img, rom_end, seeds, hmap, dasm)
    n = emit(insns, a.prefix, a.out)
    code_bytes = sum(i['size'] for i in insns.values())
    print('%s: %d instructions (%d bytes of code, %.1f%% of ROM) in %d chunks, %d seeds'
          % (a.prefix, len(insns), code_bytes, 100.0 * code_bytes / rom_end, n, len(seeds)))
    if a.listing:
        with open(a.listing, 'w') as f:
            for pc in sorted(insns):
                i = insns[pc]
                f.write('%06x  %-6s %s\n' % (pc, '%04x' % i['op'], i['text']))


if __name__ == '__main__':
    main()
