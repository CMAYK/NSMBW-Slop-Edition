#!/usr/bin/env python3
"""Static recompiler: Gekko (Wii PowerPC) machine code -> C.

Reads the original main.dol plus the dtk symbol config (function boundaries, names,
jump tables) and emits one C function per guest function into out/.

    python tools/recomp.py [--out out] [--chunk 400]

Every instruction becomes C operating on a ppc_context. Unsupported instructions are
emitted as ppc_unimplemented() calls so output always compiles; the run prints a
coverage report so we know exactly what's left.
"""
import argparse, os, re, struct, sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
SYMLINE = re.compile(r'^(\S+) = (\.[\w.]+):0x([0-9A-Fa-f]+); // (.*)$')


# --------------------------------------------------------------------------- inputs
class Image:
    """Flat guest memory image built from the DOL (and later, linked RELs)."""
    def __init__(self):
        self.segs = []   # (addr, bytes)
        self.kinds = []  # 'text' | 'data'

    def load_dol(self, path):
        d = open(path, 'rb').read()
        off = struct.unpack('>18I', d[0:72]); addr = struct.unpack('>18I', d[72:144]); sz = struct.unpack('>18I', d[144:216])
        for i in range(18):
            if sz[i]:
                self.segs.append((addr[i], d[off[i]:off[i] + sz[i]]))
                self.kinds.append('text' if i < 7 else 'data')

    def data_segs(self):
        return [(a, b) for i, (a, b) in enumerate(self.segs) if self.kinds[i] == 'data']

    def load_linked(self, linked_dir):
        """Load REL modules pre-linked by link_rels.py; returns the manifest."""
        import json
        man = json.load(open(os.path.join(linked_dir, 'manifest.json')))
        for m in man:
            img = open(os.path.join(linked_dir, m['image']), 'rb').read()
            for s in m['sections']:
                if s['kind'] == 'bss': continue
                self.segs.append((s['addr'], img[s['file_off']:s['file_off'] + s['size']]))
                self.kinds.append(s['kind'])
        return man

    def word(self, a):
        import bisect
        if getattr(self, '_starts', None) is None or len(self._starts) != len(self.segs):
            order = sorted(range(len(self.segs)), key=lambda i: self.segs[i][0])
            self.segs = [self.segs[i] for i in order]; self.kinds = [self.kinds[i] for i in order]
            self._starts = [s for s, _ in self.segs]
        i = bisect.bisect_right(self._starts, a) - 1
        if i >= 0:
            s, b = self.segs[i]
            if a + 4 <= s + len(b): return struct.unpack_from('>I', b, a - s)[0]
        return None


def load_symbols(path, sec_base=None):
    """sec_base: for REL configs, {section name: absolute address} (their symbols are section-relative)."""
    funcs, objs = [], []
    for l in open(path):
        m = SYMLINE.match(l.rstrip())
        if not m: continue
        name, sec, addr, attrs = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        sz = re.search(r'size:0x([0-9A-Fa-f]+)', attrs); sz = int(sz.group(1), 16) if sz else 0
        if sec_base is not None:
            if sec not in sec_base: continue
            addr += sec_base[sec]
        if 'type:function' in attrs and sz: funcs.append((addr, sz, name, sec))
        elif 'type:object' in attrs: objs.append((addr, sz, name, sec))
    funcs.sort(); return funcs, objs


def c_ident(name, addr):
    s = re.sub(r'[^A-Za-z0-9_]', '_', name)
    return f'{s}_{addr:08X}' if not s.endswith(f'{addr:08X}') else s


# --------------------------------------------------------------------------- decode helpers
def sx16(v): return v - 0x10000 if v & 0x8000 else v
def hx(v): return f'0x{v & 0xFFFFFFFF:X}u'


class FuncEmitter:
    def __init__(self, rc, start, size, name):
        self.rc, self.start, self.end, self.name = rc, start, start + size, name
        self.lines, self.labels = [], set()

    def inside(self, a): return self.start <= a < self.end

    # ---- branch helpers
    def cond_expr(self, bo, bi):
        parts = []
        if not (bo & 4):
            self.lines.append('    ctx->ctr--;')
            parts.append('ctx->ctr == 0' if bo & 2 else 'ctx->ctr != 0')
        if not (bo & 16):
            parts.append(f'CR_BIT(ctx, {bi})' if bo & 8 else f'!CR_BIT(ctx, {bi})')
        return ' && '.join(f'({p})' for p in parts) if parts else None

    def jump_to(self, tgt, link, pc):
        """Code for an unconditional transfer to tgt (goto / call / tail call)."""
        if link:
            return f'ctx->lr = {hx(pc + 4)}; {self.rc.call(tgt)}'
        if self.inside(tgt):
            return f'PPC_POLL(); goto L_{tgt:08X};' if tgt <= pc else f'goto L_{tgt:08X};'
        return f'{self.rc.call(tgt)} return;'

    # ---- main translation
    def translate(self):
        rc = self.rc
        words = [(a, rc.img.word(a)) for a in range(self.start, self.end, 4)]
        # pass 1: labels
        for a, w in words:
            if w is None: continue
            op = w >> 26
            if op == 18 and not (w & 1):
                li = w & 0x3FFFFFC; li = li - 0x4000000 if li & 0x2000000 else li
                t = (0 if w & 2 else a) + li
                if self.inside(t): self.labels.add(t)
            elif op == 16:
                bd = w & 0xFFFC; bd = bd - 0x10000 if bd & 0x8000 else bd
                t = (0 if w & 2 else a) + bd
                if self.inside(t): self.labels.add(t)
        for t in rc.jt_targets_in(self.start, self.end): self.labels.add(t)
        # pass 2: code
        out = self.lines
        for a, w in words:
            if a in self.labels: out.append(f'L_{a:08X}:;')
            if w is None:
                out.append(f'    /* {a:08X}: outside image */'); continue
            n0 = len(out)
            ok = self.insn(a, w)
            rc.stats['insn_total'] += 1
            if ok:
                rc.stats['insn_ok'] += 1
            else:
                rc.unimpl[ok_name(w)] += 1
                del out[n0:]
                out.append(f'    ppc_unimplemented(ctx, base, {hx(a)}, "{ok_name(w)} {w:08X}");')
        return out

    def insn(self, pc, w):
        o = self.lines.append
        op = w >> 26; D = (w >> 21) & 31; A = (w >> 16) & 31; B = (w >> 11) & 31; C = (w >> 6) & 31
        rc_bit = w & 1; simm = sx16(w & 0xFFFF); uimm = w & 0xFFFF
        rA0 = f'ctx->r[{A}]' if A else '0'          # (rA|0)
        ea_d = f'(uint32_t)({rA0} + {simm})'          # D-form EA
        ea_x = f'(uint32_t)({rA0} + ctx->r[{B}])'     # X-form EA
        r = lambda i: f'ctx->r[{i}]'
        f = lambda i: f'ctx->f[{i}].d'
        cr0 = (lambda: o(f'    CR0(ctx, ctx->r[{A}]);'))
        cr0d = (lambda: o(f'    CR0(ctx, ctx->r[{D}]);'))

        # ---------------- integer immediate
        if op == 14: o(f'    {r(D)} = (uint32_t)({rA0} + {simm});'); return True                       # addi/li
        if op == 15: o(f'    {r(D)} = (uint32_t)({rA0} + {hx(uimm << 16)});'); return True             # addis/lis
        if op == 7:  o(f'    {r(D)} = (uint32_t)((int32_t){r(A)} * {simm});'); return True             # mulli
        if op in (12, 13):                                                                              # addic(.)
            o(f'    {{ uint32_t a_ = {r(A)}; {r(D)} = a_ + {hx(simm)}; ctx->xer_ca = {r(D)} < a_; }}')
            if op == 13: cr0d()
            return True
        if op == 8:                                                                                     # subfic
            o(f'    {{ uint32_t a_ = {r(A)}; {r(D)} = {hx(simm)} - a_; ctx->xer_ca = {hx(simm)} >= a_; }}'); return True
        if op == 11:                                                                                    # cmpi
            o(f'    cr_cmp_s(ctx, {D >> 2}, (int32_t){r(A)}, {simm});'); return True
        if op == 10:                                                                                    # cmpli
            o(f'    cr_cmp_u(ctx, {D >> 2}, {r(A)}, {hx(uimm)});'); return True
        if op == 24: o(f'    {r(A)} = {r(D)} | {hx(uimm)};'); return True                               # ori (nop)
        if op == 25: o(f'    {r(A)} = {r(D)} | {hx(uimm << 16)};'); return True
        if op == 26: o(f'    {r(A)} = {r(D)} ^ {hx(uimm)};'); return True
        if op == 27: o(f'    {r(A)} = {r(D)} ^ {hx(uimm << 16)};'); return True
        if op == 28: o(f'    {r(A)} = {r(D)} & {hx(uimm)};'); cr0(); return True                       # andi.
        if op == 29: o(f'    {r(A)} = {r(D)} & {hx(uimm << 16)};'); cr0(); return True                 # andis.

        # ---------------- rotates
        if op in (20, 21, 23):
            mb, me = (w >> 6) & 31, (w >> 1) & 31
            sh = str(B) if op != 23 else f'({r(B)} & 31)'
            m = hx(mask32(mb, me))
            if op == 21:
                if B == 0: o(f'    {r(A)} = {r(D)} & {m};')
                else: o(f'    {r(A)} = rotl32({r(D)}, {sh}) & {m};')
            elif op == 23: o(f'    {r(A)} = rotl32({r(D)}, {sh}) & {m};')
            else: o(f'    {r(A)} = (rotl32({r(D)}, {sh}) & {m}) | ({r(A)} & ~{m});')
            if rc_bit: cr0()
            return True

        # ---------------- branches
        if op == 18:
            li = w & 0x3FFFFFC; li = li - 0x4000000 if li & 0x2000000 else li
            t = ((0 if w & 2 else pc) + li) & 0xFFFFFFFF
            o('    ' + self.jump_to(t, w & 1, pc)); return True
        if op == 16:
            bd = w & 0xFFFC; bd = bd - 0x10000 if bd & 0x8000 else bd
            t = ((0 if w & 2 else pc) + bd) & 0xFFFFFFFF
            c = self.cond_expr(D, A)
            j = self.jump_to(t, w & 1, pc)
            o(f'    if ({c}) {{ {j} }}' if c else '    ' + j); return True
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 16:                                                                                # bclr
                c = self.cond_expr(D, A)
                if w & 1: body = f'{{ uint32_t t_ = ctx->lr; ctx->lr = {hx(pc + 4)}; ppc_call_indirect(ctx, base, t_); }}'
                else: body = 'return;'
                o(f'    if ({c}) {body}' if c else '    ' + body); return True
            if xo == 528:                                                                               # bcctr
                c = self.cond_expr(D | 4, A)  # bcctr never decrements CTR
                if w & 1: body = f'{{ ctx->lr = {hx(pc + 4)}; ppc_call_indirect(ctx, base, ctx->ctr); }}'
                else: body = self.switch_or_tail()
                o(f'    if ({c}) {body}' if c else '    ' + body); return True
            crops = {257: '&', 449: '|', 193: '^', 225: 'nand', 33: 'nor', 289: 'eqv', 129: 'andc', 417: 'orc'}
            if xo in crops:
                a_, b_ = f'CR_BIT(ctx, {A})', f'CR_BIT(ctx, {B})'
                e = {'nand': f'!({a_} & {b_})', 'nor': f'!({a_} | {b_})', 'eqv': f'!({a_} ^ {b_})',
                     'andc': f'({a_} & !{b_})', 'orc': f'({a_} | !{b_})'}.get(crops[xo], f'({a_} {crops[xo]} {b_})')
                o(f'    CR_SETBIT(ctx, {D}, {e});'); return True
            if xo == 0: o(f'    CR_SET(ctx, {D >> 2}, (ctx->cr >> {28 - 4 * (A >> 2)}) & 0xF);'); return True  # mcrf
            if xo == 150: o('    /* isync */'); return True
            if xo == 50: o(f'    ppc_rfi(ctx, base, {hx(pc)}); return;'); return True
            return False
        if op == 17: o(f'    ppc_syscall(ctx, base, {hx(pc)});'); return True

        # ---------------- loads / stores (D-form)
        ld = {32: ('ld32', ''), 34: ('ld8', ''), 40: ('ld16', ''), 42: ('ld16', '(uint32_t)(int16_t)')}
        st = {36: 'st32', 38: 'st8', 44: 'st16'}
        if op in ld or op - 1 in ld:
            upd = op not in ld; fn, cast = ld[op - upd]
            if upd: o(f'    {{ uint32_t ea_ = {r(A)} + {simm}; {r(D)} = {cast}{fn}(base, ea_); {r(A)} = ea_; }}')
            else: o(f'    {r(D)} = {cast}{fn}(base, {ea_d});')
            return True
        if op in st or op - 1 in st:
            upd = op not in st; fn = st[op - upd]
            if upd: o(f'    {{ uint32_t ea_ = {r(A)} + {simm}; {fn}(base, ea_, {r(D)}); {r(A)} = ea_; }}')
            else: o(f'    {fn}(base, {ea_d}, {r(D)});')
            return True
        if op == 46: o(f'    {{ uint32_t ea_ = {ea_d}; for (int i_ = {D}; i_ < 32; i_++, ea_ += 4) ctx->r[i_] = ld32(base, ea_); }}'); return True
        if op == 47: o(f'    {{ uint32_t ea_ = {ea_d}; for (int i_ = {D}; i_ < 32; i_++, ea_ += 4) st32(base, ea_, ctx->r[i_]); }}'); return True
        if op in (48, 49):  # lfs(u)
            ea = f'{r(A)} + {simm}' if op == 49 else ea_d
            o(f'    {{ uint32_t ea_ = {ea}; {f(D)} = ctx->ps1[{D}] = ldfs(base, ea_);' + (f' {r(A)} = ea_;' if op == 49 else '') + ' }'); return True
        if op in (50, 51):  # lfd(u)
            ea = f'{r(A)} + {simm}' if op == 51 else ea_d
            o(f'    {{ uint32_t ea_ = {ea}; ctx->f[{D}].u = ld64(base, ea_);' + (f' {r(A)} = ea_;' if op == 51 else '') + ' }'); return True
        if op in (52, 53):
            ea = f'{r(A)} + {simm}' if op == 53 else ea_d
            o(f'    {{ uint32_t ea_ = {ea}; stfs(base, ea_, {f(D)});' + (f' {r(A)} = ea_;' if op == 53 else '') + ' }'); return True
        if op in (54, 55):
            ea = f'{r(A)} + {simm}' if op == 55 else ea_d
            o(f'    {{ uint32_t ea_ = {ea}; st64(base, ea_, ctx->f[{D}].u);' + (f' {r(A)} = ea_;' if op == 55 else '') + ' }'); return True
        if op in (56, 57, 60, 61):  # psq_l(u) / psq_st(u)
            d12 = w & 0xFFF; d12 = d12 - 0x1000 if d12 & 0x800 else d12
            W, I = (w >> 15) & 1, (w >> 12) & 7
            upd = op in (57, 61)
            ea = f'{r(A)} + {d12}' if upd else f'(uint32_t)({rA0} + {d12})'
            fn = 'psq_l' if op < 60 else 'psq_st'
            o(f'    {{ uint32_t ea_ = {ea}; {fn}(ctx, base, {D}, ea_, {W}, {I});' + (f' {r(A)} = ea_;' if upd else '') + ' }'); return True

        if op == 31: return self.op31(pc, w, D, A, B, rc_bit, rA0, ea_x)
        if op == 59: return self.op59(w, D, A, B, C, rc_bit)
        if op == 63: return self.op63(w, D, A, B, C, rc_bit)
        if op == 4:  return self.op4(w, D, A, B, C, rA0)
        if op == 3:  o(f'    ppc_trap(ctx, base, {hx(pc)});'); return True  # twi
        return False

    def switch_or_tail(self):
        tg = sorted(self.rc.jt_targets_in(self.start, self.end))
        if not tg: return '{ ppc_call_indirect(ctx, base, ctx->ctr); return; }'
        cases = ' '.join(f'case {hx(t)}: goto L_{t:08X};' for t in tg)
        return f'{{ switch (ctx->ctr) {{ {cases} default: ppc_call_indirect(ctx, base, ctx->ctr); return; }} }}'

    # ---------------------------------------------------------------- opcode 31
    def op31(self, pc, w, D, A, B, rc, rA0, ea):
        o = self.lines.append; r = lambda i: f'ctx->r[{i}]'
        xo = (w >> 1) & 0x3FF; xo9 = (w >> 1) & 0x1FF
        def crd(): rc and o(f'    CR0(ctx, ctx->r[{D}]);')
        def cra(): rc and o(f'    CR0(ctx, ctx->r[{A}]);')
        arith = {266: f'{r(A)} + {r(B)}', 40: f'{r(B)} - {r(A)}', 235: f'(uint32_t)((int32_t){r(A)} * (int32_t){r(B)})',
                 75: f'(uint32_t)(((int64_t)(int32_t){r(A)} * (int32_t){r(B)}) >> 32)',
                 11: f'(uint32_t)(((uint64_t){r(A)} * {r(B)}) >> 32)',
                 491: f'divw((int32_t){r(A)}, (int32_t){r(B)})', 459: f'divwu({r(A)}, {r(B)})',
                 104: f'(uint32_t)-(int32_t){r(A)}'}
        if xo9 in arith: o(f'    {r(D)} = {arith[xo9]};'); crd(); return True
        carry = {10: (f'{r(A)}', f'{r(B)}', '0'), 8: (f'~{r(A)}', f'{r(B)}', '1'),
                 138: (f'{r(A)}', f'{r(B)}', 'ctx->xer_ca'), 136: (f'~{r(A)}', f'{r(B)}', 'ctx->xer_ca'),
                 202: (f'{r(A)}', '0', 'ctx->xer_ca'), 200: (f'~{r(A)}', '0', 'ctx->xer_ca'),
                 234: (f'{r(A)}', '0xFFFFFFFFu', 'ctx->xer_ca'), 232: (f'~{r(A)}', '0xFFFFFFFFu', 'ctx->xer_ca')}
        if xo9 in carry:
            a_, b_, c_ = carry[xo9]
            o(f'    {{ uint64_t t_ = (uint64_t)(uint32_t)({a_}) + (uint32_t)({b_}) + {c_}; {r(D)} = (uint32_t)t_; ctx->xer_ca = (uint32_t)(t_ >> 32); }}')
            crd(); return True
        logic = {28: '&', 444: '|', 316: '^', 60: '& ~', 412: '| ~'}
        if xo in logic: o(f'    {r(A)} = {r(D)} {logic[xo]}{r(B)};'); cra(); return True
        if xo == 124: o(f'    {r(A)} = ~({r(D)} | {r(B)});'); cra(); return True
        if xo == 476: o(f'    {r(A)} = ~({r(D)} & {r(B)});'); cra(); return True
        if xo == 284: o(f'    {r(A)} = ~({r(D)} ^ {r(B)});'); cra(); return True
        if xo == 24: o(f'    {{ uint32_t s_ = {r(B)} & 63; {r(A)} = s_ > 31 ? 0 : {r(D)} << s_; }}'); cra(); return True
        if xo == 536: o(f'    {{ uint32_t s_ = {r(B)} & 63; {r(A)} = s_ > 31 ? 0 : {r(D)} >> s_; }}'); cra(); return True
        if xo == 792:
            o(f'    {{ uint32_t s_ = {r(B)} & 63; int32_t v_ = (int32_t){r(D)}; uint32_t res_ = s_ > 31 ? (uint32_t)(v_ >> 31) : (uint32_t)(v_ >> s_);'
              f' ctx->xer_ca = v_ < 0 && s_ != 0 && (s_ > 31 || ((uint32_t)v_ & ((1u << s_) - 1)) != 0); {r(A)} = res_; }}'); cra(); return True
        if xo == 824:
            sh = B
            m = hx((1 << sh) - 1) if sh else '0u'
            o(f'    {{ int32_t v_ = (int32_t){r(D)}; ctx->xer_ca = v_ < 0 && ((uint32_t)v_ & {m}) != 0; {r(A)} = (uint32_t)(v_ >> {sh}); }}'); cra(); return True
        if xo == 26: o(f'    {r(A)} = cntlzw({r(D)});'); cra(); return True
        if xo == 922: o(f'    {r(A)} = (uint32_t)(int16_t){r(D)};'); cra(); return True
        if xo == 954: o(f'    {r(A)} = (uint32_t)(int8_t){r(D)};'); cra(); return True
        if xo == 0: o(f'    cr_cmp_s(ctx, {D >> 2}, (int32_t){r(A)}, (int32_t){r(B)});'); return True
        if xo == 32: o(f'    cr_cmp_u(ctx, {D >> 2}, {r(A)}, {r(B)});'); return True
        # loads/stores indexed
        ldx = {23: ('ld32', '', 0), 55: ('ld32', '', 1), 87: ('ld8', '', 0), 119: ('ld8', '', 1),
               279: ('ld16', '', 0), 311: ('ld16', '', 1), 343: ('ld16', '(uint32_t)(int16_t)', 0), 375: ('ld16', '(uint32_t)(int16_t)', 1),
               534: ('BSWAP32(ld32', ')', 0), 790: ('(uint32_t)BSWAP16(ld16', ')', 0), 20: ('ld32', '', 0)}
        if xo in ldx:
            fn, tail, upd = ldx[xo]
            if fn.startswith(('BSWAP', '(uint32_t)BSWAP')):
                o(f'    {r(D)} = {fn}(base, {ea}){tail};')
            elif upd: o(f'    {{ uint32_t ea_ = {r(A)} + {r(B)}; {r(D)} = {tail}{fn}(base, ea_); {r(A)} = ea_; }}')
            else: o(f'    {r(D)} = {tail}{fn}(base, {ea});')
            if xo == 20: o(f'    ctx->reserve = {ea};')
            return True
        stx = {151: ('st32', 0), 183: ('st32', 1), 215: ('st8', 0), 247: ('st8', 1), 407: ('st16', 0), 439: ('st16', 1)}
        if xo in stx:
            fn, upd = stx[xo]
            if upd: o(f'    {{ uint32_t ea_ = {r(A)} + {r(B)}; {fn}(base, ea_, {r(D)}); {r(A)} = ea_; }}')
            else: o(f'    {fn}(base, {ea}, {r(D)});')
            return True
        if xo == 662: o(f'    st32(base, {ea}, BSWAP32({r(D)}));'); return True
        if xo == 918: o(f'    st16(base, {ea}, BSWAP16((uint16_t){r(D)}));'); return True
        if xo == 150: o(f'    st32(base, {ea}, {r(D)}); CR_SET(ctx, 0, 2 | (ctx->xer_so & 1));'); return True  # stwcx.
        # FP indexed
        if xo in (535, 567):
            upd = xo == 567
            o(f'    {{ uint32_t ea_ = {"ctx->r[%d] + ctx->r[%d]" % (A, B) if upd else ea}; ctx->f[{D}].d = ctx->ps1[{D}] = ldfs(base, ea_);' + (f' {r(A)} = ea_;' if upd else '') + ' }'); return True
        if xo in (599, 631):
            upd = xo == 631
            o(f'    {{ uint32_t ea_ = {"ctx->r[%d] + ctx->r[%d]" % (A, B) if upd else ea}; ctx->f[{D}].u = ld64(base, ea_);' + (f' {r(A)} = ea_;' if upd else '') + ' }'); return True
        if xo in (663, 695):
            upd = xo == 695
            o(f'    {{ uint32_t ea_ = {"ctx->r[%d] + ctx->r[%d]" % (A, B) if upd else ea}; stfs(base, ea_, ctx->f[{D}].d);' + (f' {r(A)} = ea_;' if upd else '') + ' }'); return True
        if xo in (727, 759):
            upd = xo == 759
            o(f'    {{ uint32_t ea_ = {"ctx->r[%d] + ctx->r[%d]" % (A, B) if upd else ea}; st64(base, ea_, ctx->f[{D}].u);' + (f' {r(A)} = ea_;' if upd else '') + ' }'); return True
        if xo == 983: o(f'    st32(base, {ea}, (uint32_t)ctx->f[{D}].u);'); return True  # stfiwx
        # SPRs
        if xo in (339, 467, 371):
            spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
            name = {1: None, 8: 'ctx->lr', 9: 'ctx->ctr', 268: None, 269: None}.get(spr, 'x')
            if 912 <= spr <= 919: name = f'ctx->gqr[{spr - 912}]'
            elif name == 'x': name = f'ctx->spr[{spr}]'
            if spr == 22:
                o(f'    {r(D)} = ppc_get_dec(ctx);' if xo == 339 else f'    ppc_set_dec(ctx, {r(D)});'); return True
            if xo == 467 and spr == 923:          # DMA_L: locked-cache <-> memory DMA
                o(f'    ppc_set_dma_l(ctx, base, {r(D)});'); return True
            if xo == 467 and spr in (284, 285):
                o(f'    ppc_set_tb(ctx, {spr - 284}, {r(D)});'); return True
            if xo == 371 or spr in (268, 269):
                part = '(uint32_t)ppc_timebase()' if spr == 268 else '(uint32_t)(ppc_timebase() >> 32)'
                o(f'    {r(D)} = {part};'); return True
            if xo == 339:
                if spr == 1: o(f'    {r(D)} = (ctx->xer_so << 31) | (ctx->xer_ov << 30) | (ctx->xer_ca << 29) | ctx->xer_bc;')
                else: o(f'    {r(D)} = {name};')
            else:
                if spr == 1: o(f'    {{ uint32_t x_ = {r(D)}; ctx->xer_so = x_ >> 31; ctx->xer_ov = (x_ >> 30) & 1; ctx->xer_ca = (x_ >> 29) & 1; ctx->xer_bc = x_ & 0x7F; }}')
                else: o(f'    {name} = {r(D)};')
            return True
        if xo == 19: o(f'    {r(D)} = ctx->cr;'); return True
        if xo == 144:
            crm = (w >> 12) & 0xFF; m = 0
            for i in range(8):
                if crm & (0x80 >> i): m |= 0xF << (28 - 4 * i)
            o(f'    ctx->cr = (ctx->cr & ~{hx(m)}) | ({r(D)} & {hx(m)});'); return True
        if xo == 83: o(f'    {r(D)} = ctx->msr;'); return True
        if xo == 146: o(f'    ctx->msr = {r(D)};'); return True
        if xo == 512: o(f'    CR_SET(ctx, {D >> 2}, (ctx->xer_so << 3) | (ctx->xer_ov << 2) | (ctx->xer_ca << 1)); ctx->xer_so = ctx->xer_ov = ctx->xer_ca = 0;'); return True
        if xo == 1014: o(f'    ppc_dcbz(base, {ea});'); return True  # dcbz
        if xo in (54, 86, 470, 982, 246, 278, 598, 854): o('    /* cache/sync */'); return True
        if xo == 4: o(f'    ppc_trap(ctx, base, {hx(pc)});'); return True
        if xo == 595 or xo == 210 or xo == 242: o(f'    /* segment reg {xo} */'); return True
        return False

    # ---------------------------------------------------------------- FP
    def op59(self, w, D, A, B, C, rc):
        o = self.lines.append; f = lambda i: f'ctx->f[{i}].d'
        xo = (w >> 1) & 31
        ex = {21: f'{f(A)} + {f(B)}', 20: f'{f(A)} - {f(B)}', 25: f'{f(A)} * {f(C)}', 18: f'{f(A)} / {f(B)}',
              29: f'{f(A)} * {f(C)} + {f(B)}', 28: f'{f(A)} * {f(C)} - {f(B)}',
              31: f'-({f(A)} * {f(C)} + {f(B)})', 30: f'-({f(A)} * {f(C)} - {f(B)})', 24: f'fres({f(B)})'}
        if xo in ex: o(f'    {f(D)} = ctx->ps1[{D}] = frsp({ex[xo]});'); return not rc or self.fcr1()
        return False

    def fcr1(self):
        self.lines.append('    CR_SET(ctx, 1, (ctx->fpscr >> 28) & 0xF);'); return True

    def op63(self, w, D, A, B, C, rc):
        o = self.lines.append; f = lambda i: f'ctx->f[{i}].d'
        xo5 = (w >> 1) & 31; xo = (w >> 1) & 0x3FF
        ex = {21: f'{f(A)} + {f(B)}', 20: f'{f(A)} - {f(B)}', 25: f'{f(A)} * {f(C)}', 18: f'{f(A)} / {f(B)}',
              29: f'{f(A)} * {f(C)} + {f(B)}', 28: f'{f(A)} * {f(C)} - {f(B)}',
              31: f'-({f(A)} * {f(C)} + {f(B)})', 30: f'-({f(A)} * {f(C)} - {f(B)})',
              26: f'frsqrte({f(B)})', 23: f'({f(A)} >= 0.0) ? {f(C)} : {f(B)}'}
        if xo5 in ex: o(f'    {f(D)} = {ex[xo5]};'); return not rc or self.fcr1()
        if xo in (0, 32): o(f'    cr_fcmp(ctx, {D >> 2}, {f(A)}, {f(B)});'); return True
        if xo == 12: o(f'    {f(D)} = frsp({f(B)});'); return not rc or self.fcr1()
        if xo in (14, 15): o(f'    ctx->f[{D}].u = fctiw_bits({f(B)}, {int(xo == 15)});'); return not rc or self.fcr1()
        if xo == 72: o(f'    ctx->f[{D}].u = ctx->f[{B}].u;'); return not rc or self.fcr1()
        if xo == 40: o(f'    ctx->f[{D}].u = ctx->f[{B}].u ^ 0x8000000000000000ull;'); return not rc or self.fcr1()
        if xo == 264: o(f'    ctx->f[{D}].u = ctx->f[{B}].u & ~0x8000000000000000ull;'); return not rc or self.fcr1()
        if xo == 136: o(f'    ctx->f[{D}].u = ctx->f[{B}].u | 0x8000000000000000ull;'); return not rc or self.fcr1()
        if xo == 583: o(f'    ctx->f[{D}].u = 0xFFF8000000000000ull | ctx->fpscr;'); return True                   # mffs
        if xo == 711:                                                                                               # mtfsf
            fm = (w >> 17) & 0xFF; m = 0
            for i in range(8):
                if fm & (0x80 >> i): m |= 0xF << (28 - 4 * i)
            o(f'    ctx->fpscr = (ctx->fpscr & ~{hx(m)}) | ((uint32_t)ctx->f[{B}].u & {hx(m)});'); return True
        if xo == 38: o(f'    ctx->fpscr |= {hx(1 << (31 - D))};'); return True
        if xo == 70: o(f'    ctx->fpscr &= ~{hx(1 << (31 - D))};'); return True
        if xo == 134:
            imm = (w >> 12) & 0xF; sh = 28 - 4 * (D >> 2)
            o(f'    ctx->fpscr = (ctx->fpscr & ~{hx(0xF << sh)}) | {hx(imm << sh)};'); return True
        if xo == 64: o(f'    CR_SET(ctx, {D >> 2}, (ctx->fpscr >> {28 - 4 * (A >> 2)}) & 0xF);'); return True
        return False

    # ---------------------------------------------------------------- paired singles
    def op4(self, w, D, A, B, C, rA0):
        o = self.lines.append
        p0 = lambda i: f'ctx->f[{i}].d'; p1 = lambda i: f'ctx->ps1[{i}]'
        xo6 = (w >> 1) & 0x3F; xo5 = (w >> 1) & 31; xo = (w >> 1) & 0x3FF
        if xo6 in (6, 7, 38, 39):  # psq_lx / psq_stx / psq_lux / psq_stux
            W, I = (w >> 10) & 1, (w >> 7) & 7
            upd = xo6 >= 38
            ea = f'ctx->r[{A}] + ctx->r[{B}]' if upd else f'(uint32_t)({rA0} + ctx->r[{B}])'
            fn = 'psq_l' if xo6 in (6, 38) else 'psq_st'
            o(f'    {{ uint32_t ea_ = {ea}; {fn}(ctx, base, {D}, ea_, {W}, {I});' + (f' ctx->r[{A}] = ea_;' if upd else '') + ' }'); return True
        def pair(e0, e1, rnd=True):
            w0, w1 = (f'frsp({e0})', f'frsp({e1})') if rnd else (e0, e1)
            o(f'    {{ double a_ = {w0}, b_ = {w1}; {p0(D)} = a_; {p1(D)} = b_; }}'); return True
        if xo5 == 21: return pair(f'{p0(A)} + {p0(B)}', f'{p1(A)} + {p1(B)}')
        if xo5 == 20: return pair(f'{p0(A)} - {p0(B)}', f'{p1(A)} - {p1(B)}')
        if xo5 == 25: return pair(f'{p0(A)} * {p0(C)}', f'{p1(A)} * {p1(C)}')
        if xo5 == 18: return pair(f'{p0(A)} / {p0(B)}', f'{p1(A)} / {p1(B)}')
        if xo5 == 29: return pair(f'{p0(A)} * {p0(C)} + {p0(B)}', f'{p1(A)} * {p1(C)} + {p1(B)}')
        if xo5 == 28: return pair(f'{p0(A)} * {p0(C)} - {p0(B)}', f'{p1(A)} * {p1(C)} - {p1(B)}')
        if xo5 == 31: return pair(f'-({p0(A)} * {p0(C)} + {p0(B)})', f'-({p1(A)} * {p1(C)} + {p1(B)})')
        if xo5 == 30: return pair(f'-({p0(A)} * {p0(C)} - {p0(B)})', f'-({p1(A)} * {p1(C)} - {p1(B)})')
        if xo5 == 12: return pair(f'{p0(A)} * {p0(C)}', f'{p1(A)} * {p0(C)}')
        if xo5 == 13: return pair(f'{p0(A)} * {p1(C)}', f'{p1(A)} * {p1(C)}')
        if xo5 == 14: return pair(f'{p0(A)} * {p0(C)} + {p0(B)}', f'{p1(A)} * {p0(C)} + {p1(B)}')
        if xo5 == 15: return pair(f'{p0(A)} * {p1(C)} + {p0(B)}', f'{p1(A)} * {p1(C)} + {p1(B)}')
        if xo5 == 10: return pair(f'{p0(A)} + {p1(B)}', p1(C))
        if xo5 == 11: return pair(p0(C), f'{p0(A)} + {p1(B)}')
        if xo5 == 23: return pair(f'({p0(A)} >= 0.0) ? {p0(C)} : {p0(B)}', f'({p1(A)} >= 0.0) ? {p1(C)} : {p1(B)}', False)
        if xo5 == 24: return pair(f'1.0 / {p0(B)}', f'1.0 / {p1(B)}')
        if xo5 == 26: return pair(f'1.0 / sqrt({p0(B)})', f'1.0 / sqrt({p1(B)})')
        if xo == 72: return pair(p0(B), p1(B), False)
        if xo == 40: return pair(f'-{p0(B)}', f'-{p1(B)}', False)
        if xo == 264: return pair(f'fabs({p0(B)})', f'fabs({p1(B)})', False)
        if xo == 136: return pair(f'-fabs({p0(B)})', f'-fabs({p1(B)})', False)
        if xo == 528: return pair(p0(A), p0(B), False)
        if xo == 560: return pair(p0(A), p1(B), False)
        if xo == 592: return pair(p1(A), p0(B), False)
        if xo == 624: return pair(p1(A), p1(B), False)
        if xo in (0, 32): o(f'    cr_fcmp(ctx, {D >> 2}, {p0(A)}, {p0(B)});'); return True
        if xo in (64, 96): o(f'    cr_fcmp(ctx, {D >> 2}, {p1(A)}, {p1(B)});'); return True
        if xo == 1014: o(f'    ppc_dcbz(base, (uint32_t)({rA0} + ctx->r[{B}]));'); return True  # dcbz_l
        return False


def mask32(mb, me):
    m1 = 0xFFFFFFFF >> mb; m2 = (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return (m1 & m2) if mb <= me else (m1 | m2)


def ok_name(w):
    op = w >> 26
    if op in (4, 19, 31, 59, 63): return f'op{op}/xo{(w >> 1) & 0x3FF}'
    return f'op{op}'


# --------------------------------------------------------------------------- driver
class Recompiler:
    def __init__(self, img, funcs, objs):
        funcs = self.add_secondary_entries(img, funcs)
        self.img, self.funcs = img, funcs
        self.by_addr = {a: (a, s, n) for a, s, n, _ in funcs}
        self.starts = sorted(self.by_addr)
        self.stats, self.unimpl = Counter(), Counter()
        self.missing_targets = Counter()
        # Jump tables, found structurally rather than by name: any data word that points
        # into the interior of a function containing a computed jump (bctr) is a switch case.
        import bisect
        bctr_funcs = []
        for a, s, n, _ in funcs:
            if any(img.word(x) == 0x4E800420 for x in range(a, a + s, 4)): bctr_funcs.append((a, a + s))
        bstarts = [b[0] for b in bctr_funcs]
        jt = set()
        for seg_addr, seg in img.data_segs():
            for j in range(0, len(seg) - 3, 4):
                v = struct.unpack_from('>I', seg, j)[0]
                i = bisect.bisect_right(bstarts, v) - 1
                if i >= 0 and bctr_funcs[i][0] < v < bctr_funcs[i][1] and not v & 3: jt.add(v)
        self.jt = sorted(jt)
        self.ident = {a: c_ident(n, a) for a, s, n, _ in funcs}

    @staticmethod
    def add_secondary_entries(img, funcs):
        """Functions with several entry points (CodeWarrior's __save_gpr/__restore_gpr and
        friends): every direct call/branch landing inside another function becomes its own
        function running from that address to the parent's end."""
        import bisect
        funcs = sorted(funcs); starts = [f[0] for f in funcs]; known = set(starts)
        extra = {}
        for a0, sz, name, sec in funcs:
            for a in range(a0, a0 + sz, 4):
                w = img.word(a)
                if w is None or w >> 26 != 18: continue
                li = w & 0x3FFFFFC; li -= 0x4000000 if li & 0x2000000 else 0
                t = ((0 if w & 2 else a) + li) & 0xFFFFFFFF
                if t in known or (a0 <= t < a0 + sz and not w & 1): continue
                i = bisect.bisect_right(starts, t) - 1
                if i < 0: continue
                p = funcs[i]
                if p[0] < t < p[0] + p[1] and t not in extra:
                    extra[t] = (t, p[0] + p[1] - t, f'{p[2]}_entry', p[3])
        return sorted(funcs + list(extra.values()))

    def jt_targets_in(self, s, e):
        import bisect
        i = bisect.bisect_left(self.jt, s); out = set()
        while i < len(self.jt) and self.jt[i] < e: out.add(self.jt[i]); i += 1
        return out

    def load_hle(self, path):
        """Functions listed in runtime/hle_funcs.txt are replaced by hand-written hle_<name>();
        the recompiled original stays available as orig_<name>()."""
        self.hle = set()
        if os.path.exists(path):
            for l in open(path):
                l = l.split('#')[0].strip()
                if l: self.hle.add(l)
        self.body_ident = dict(self.ident)
        found = set()
        for a, s, n, _ in self.funcs:
            if n in self.hle:
                self.body_ident[a] = f'orig_{n}'; self.ident[a] = f'hle_{n}'; found.add(n)
        return self.hle - found

    def call(self, tgt):
        if tgt in self.ident: return f'{self.ident[tgt]}(ctx, base);'
        self.missing_targets[tgt] += 1
        return f'ppc_call_indirect(ctx, base, {hx(tgt)});'

    def run(self, outdir, chunk):
        os.makedirs(outdir, exist_ok=True)
        import io
        class Out(io.StringIO):
            # write the file only if its content changed, so unchanged chunks keep their objects
            def __init__(self, path): super().__init__(); self.path = path
            def __enter__(self): return self
            def __exit__(self, *a):
                data = self.getvalue()
                try:
                    with open(self.path) as f: same = f.read() == data
                except OSError: same = False
                if not same:
                    with open(self.path, 'w') as f: f.write(data)
                return False
        with Out(os.path.join(outdir, 'recomp_funcs.h')) as h:
            h.write('#pragma once\n#include "ppc_context.h"\n')
            for a, s, n, _ in self.funcs:
                h.write(f'void {self.body_ident[a]}(ppc_context *ctx, uint8_t *base);\n')
                if self.body_ident[a] != self.ident[a]: h.write(f'void {self.ident[a]}(ppc_context *ctx, uint8_t *base);\n')
            h.write('typedef struct { uint32_t addr; ppc_func fn; } ppc_func_entry;\n')
            h.write('extern const ppc_func_entry ppc_func_table[];\nextern const uint32_t ppc_func_count;\n')
            h.write('typedef struct { uint32_t addr, size; const char *name; } ppc_func_name;\n')
            h.write('extern const ppc_func_name ppc_func_names[];\n')
        with Out(os.path.join(outdir, 'recomp_table.c')) as t:
            t.write('#include "recomp_funcs.h"\nconst ppc_func_entry ppc_func_table[] = {\n')
            for a, s, n, _ in self.funcs: t.write(f'    {{ 0x{a:08X}u, {self.ident[a]} }},\n')
            t.write(f'}};\nconst uint32_t ppc_func_count = {len(self.funcs)};\n')
            t.write('const ppc_func_name ppc_func_names[] = {\n')
            for a, s, n, _ in self.funcs: t.write(f'    {{ 0x{a:08X}u, 0x{s:X}u, "{n}" }},\n')
            t.write('};\n')
        for ci in range(0, len(self.funcs), chunk):
            with Out(os.path.join(outdir, f'recomp_{ci // chunk:03d}.c')) as fo:
                fo.write('#include "recomp_funcs.h"\n\n')
                for a, s, n, sec in self.funcs[ci:ci + chunk]:
                    fe = FuncEmitter(self, a, s, n)
                    body = fe.translate()
                    fo.write(f'/* {n} @ {a:08X} size {s:#x} */\n')
                    fo.write(f'void {self.body_ident[a]}(ppc_context *ctx, uint8_t *base) {{\n    PPC_ENTER(0x{a:08X}u);\n')
                    fo.write('\n'.join(body))
                    fo.write('\n}\n\n')
                    self.stats['funcs'] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'out'))
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--no-rels', action='store_true', help='main.dol only')
    a = ap.parse_args()
    img = Image(); img.load_dol(os.path.join(ROOT, 'orig', 'main.dol'))
    funcs, objs = load_symbols(os.path.join(ROOT, 'config', 'symbols.txt'))
    funcs = [f for f in funcs if f[3] in ('.init', '.text')]
    if not a.no_rels:
        man = img.load_linked(os.path.join(ROOT, 'linked'))
        for m in man:
            base = {s['name']: s['addr'] for s in m['sections']}
            f2, o2 = load_symbols(os.path.join(ROOT, 'config', m['name'], 'symbols.txt'), base)
            funcs += [f for f in f2 if f[3] == '.text']; objs += o2
        funcs.sort()
    rc = Recompiler(img, funcs, objs)
    missing = rc.load_hle(os.path.join(ROOT, 'runtime', 'hle_funcs.txt'))
    if missing: print('WARNING: HLE names not found:', sorted(missing))
    rc.run(a.out, a.chunk)
    s = rc.stats
    print(f"functions: {s['funcs']}  instructions: {s['insn_total']}  translated: {s['insn_ok']} "
          f"({100.0 * s['insn_ok'] / max(1, s['insn_total']):.3f}%)")
    print('jump-table targets:', len(rc.jt), ' calls to non-function-start addresses:', len(rc.missing_targets))
    print('unimplemented by opcode:')
    for k, v in rc.unimpl.most_common(30): print(f'  {k:16s} {v}')


if __name__ == '__main__':
    main()
