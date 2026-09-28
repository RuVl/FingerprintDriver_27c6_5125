#!/usr/bin/env python3
"""Structure an AlgoChicago rule tree (compare/branch code over a score record) into readable C.

    ../../.venv/bin/python lift_rules.py <func_va> <name> params <flag..> [OFF=VAL ...] [--final]

This is a reverse-engineering aid for stage 5 (docs/stage5-match.md), like a decompiler: it
symbolically executes the function once (registers hold expressions over the record fields),
folds the template type (params+0x3c) and other given params fields to constants, removes the
branches that are dead for that profile, and rebuilds the remaining decision DAG as nested
if/else (structured by immediate post-dominators), then flattens it into `if (a && b) x = v;`
rules (a rule = the cone of decisions before one flag store and the post-dominating point where
the other paths continue; compiler-merged `store; jump` tails are split per predecessor first).
Stores go only to the flag pointers given as arguments.  The output is written in terms of
the port's GoodixChicagoMatchScoreRecord field names and is checked against the DLL function
itself by reeval_difftest.c (random + boundary records through winpe).

Arguments: rcx = record, rdx = params, r8/r9 = flag pointers named by <flag..>; OFF=VAL folds
params+OFF (hex) to a constant; --final prints one condition per final flag value instead of the
sequential rules.  Stage 5 used:
    lift_rules.py 0x180019a30 match_reeval_conf_tree_type24 params conf 3c=24 40=1 44=0 0=0 4=0
    lift_rules.py 0x18001fdd0 match_reeval_status_tree_type24 params conf status 3c=24 40=1 44=0 0=0 4=0
"""
import re
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from fwre.pe import Pe  # noqa: E402
import capstone  # noqa: E402
from capstone import x86  # noqa: E402

DLL = pathlib.Path(__file__).resolve().parents[2] / 'win-driver' / 'AlgoChicago.dll'
REC = {0x00: 'geometry_count', 0x04: 'secondary_geometry_count', 0x08: 'reserved08[0]',
       0x0c: 'reserved08[1]', 0x10: 'selector', 0x14: 'agreement', 0x18: 'study_metric_18',
       0x1c: 'study_metric_1c', 0x20: 'study_metric_20', 0x24: 'normalized_coverage',
       0x28: 'matched_percent', 0x2c: 'geometry_percent', 0x30: 'penalty_flag_a',
       0x34: 'penalty_flag_b', 0x54: 'probe_quality', 0x58: 'probe_coverage',
       0x5c: 'gallery_quality', 0x60: 'gallery_coverage', 0x64: 'auxiliary_score'}
for k in range(7):
    REC[0x38 + 4 * k] = f'reserved38[{k}]'

R64 = {}
for full in ['rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp']:
    b = full[1:]
    for n in (full, 'e' + b, b):
        R64[n] = full
R64.update({'al': 'rax', 'bl': 'rbx', 'cl': 'rcx', 'dl': 'rdx', 'sil': 'rsi', 'dil': 'rdi'})
for i in range(8, 16):
    for suf in ('', 'd', 'w', 'b'):
        R64[f'r{i}{suf}'] = f'r{i}'


# ---- expressions: int constants, or ('v', text) / ('ptr', base, off) --------------------------
def is_c(e):
    return isinstance(e, int)


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


def txt(e):
    if is_c(e):
        return str(s32(e))
    if e[0] == 'ptr':
        raise ValueError(f'pointer used as a value: {e}')
    return e[1]


def atom(e):
    t = txt(e)
    return t if is_c(e) or e[2] else f'({t})'


def V(t, simple=False):
    return ('v', t, simple)


def add(a, b, neg=False):
    if is_c(a) and is_c(b):
        return s32(a - b if neg else a + b)
    if is_c(b) and b == 0:
        return a
    if is_c(a) and a == 0 and not neg:
        return b
    if isinstance(a, tuple) and a[0] == 'ptr' and is_c(b):
        return ('ptr', a[1], a[2] - b if neg else a[2] + b)
    if not neg and not is_c(a) and not is_c(b):   # x + k * x -> (k + 1) * x
        m = re.fullmatch(r'(\d+) \* (.*)', txt(b))
        if m and m.group(2) == atom(a):
            return mul(a, int(m.group(1)) + 1)
    return V(f'{txt(a)} {"-" if neg else "+"} {atom(b)}')


def mul(a, k):
    if is_c(a):
        return s32(a * k)
    if k == 1:
        return a
    return V(f'{k} * {atom(a)}')


class Machine:
    def __init__(self, pe, args, fold, track=False):
        self.pe, self.fold, self.track = pe, fold, track
        self.reg = {r: ('saved', r) for r in set(R64.values())}
        self.reg.update({'rcx': ('ptr', 'REC', 0), 'rdx': ('ptr', 'PAR', 0), 'rsp': ('ptr', 'STACK', 0)})
        for r, name in zip(['r8', 'r9'], args):
            self.reg[r] = ('ptr', 'FLAG:' + name, 0)
        self.stack = {}
        self.flags = None
        self.mem = {name: V(f'{name}_in', True) for name in args}   # flag values (tracked)

    def clone(self):
        m = Machine.__new__(Machine)
        m.pe, m.fold, m.track = self.pe, self.fold, self.track
        m.reg, m.stack, m.flags = dict(self.reg), dict(self.stack), None
        m.mem = dict(self.mem)
        return m

    def key(self, live=None):
        regs = tuple(sorted((k, repr(v)) for k, v in self.reg.items() if live is None or k in live))
        stk = tuple(sorted((k, repr(v)) for k, v in self.stack.items()))
        return regs, stk, tuple(sorted((k, repr(v)) for k, v in self.mem.items()))   # flags are dead at block starts (checked: a jcc on None fails)

    # memory operand -> ('ptr', base, off) address
    def addr(self, op):
        m = op.mem
        a = self.reg[R64[self.pe.md.reg_name(m.base)]] if m.base else 0
        if m.index:
            a = add(a, mul(self.reg[R64[self.pe.md.reg_name(m.index)]], m.scale))
        return add(a, m.disp)

    def load(self, op):
        a = self.addr(op)
        if not (isinstance(a, tuple) and a[0] == 'ptr'):
            raise ValueError(f'load from non-pointer {a}')
        base, off = a[1], a[2]
        if base == 'REC':
            return V(f'rec->{REC[off]}', True)
        if base == 'PAR':
            if off in self.fold:
                return self.fold[off]
            return V(f'params_{off:02x}', True)
        if base.startswith('FLAG:'):
            assert off == 0
            return self.mem[base[5:]] if self.track else V('*' + base[5:], True)
        if base == 'STACK':
            if off not in self.stack:
                raise ValueError(f'read of unset stack slot {off:#x}')
            return self.stack[off]
        raise ValueError(a)

    def val(self, op):
        if op.type == x86.X86_OP_IMM:
            return s32(op.imm)
        if op.type == x86.X86_OP_REG:
            return self.reg[R64[self.pe.md.reg_name(op.reg)]]
        return self.load(op)

    def setreg(self, op, v):
        self.reg[R64[self.pe.md.reg_name(op.reg)]] = v


NEG = {'==': '!=', '!=': '==', '<': '>=', '>=': '<', '>': '<=', '<=': '>'}


TEMPS = {}   # compound operand text -> temp name (hoisted as const gint32 at the top)


def opnd(e):
    if is_c(e) or e[0] != 'v' or e[2]:
        return txt(e)
    t = txt(e)
    assert '*' not in t.replace(' * ', ''), t
    if t not in TEMPS:
        TEMPS[t] = f't{len(TEMPS)}'
    return TEMPS[t]


def cond_of(flags, mn):
    """condition text for jcc/setcc mnemonic suffix; returns int (0/1) when constant."""
    if flags is None:
        raise ValueError(f'{mn} consumes flags set in another block')
    kind, a, b = flags
    cc = mn[1:] if mn.startswith('j') else mn[3:]
    if kind == 'bt':
        if is_c(a) and is_c(b):
            bit = (a >> (b & 31)) & 1
            return {'b': bit, 'ae': 1 - bit, 'c': bit, 'nc': 1 - bit}[cc]
        raise ValueError('symbolic bt')
    if kind == 'test':
        a = a if (repr(a) == repr(b)) else (a & b if is_c(a) and is_c(b) else V(f'{atom(a)} & {atom(b)}'))
        b = 0
    if kind == 'res':
        b = 0
    ops = {'e': '==', 'z': '==', 'ne': '!=', 'nz': '!=', 'g': '>', 'ge': '>=', 'l': '<', 'le': '<=',
           's': '<', 'ns': '>='}
    uops = {'a': '>', 'ae': '>=', 'b': '<', 'be': '<='}
    if cc in ops:
        op = ops[cc]
        if cc in ('s', 'ns'):
            b = 0
        if is_c(a) and is_c(b):
            return int(eval(f'{s32(a)} {op} {s32(b)}'))
        return f'{opnd(a)} {op} {opnd(b)}'
    if cc in uops:
        op = uops[cc]
        if is_c(a) and is_c(b):
            return int(eval(f'{a & 0xffffffff} {op} {b & 0xffffffff}'))
        return f'(guint32) {atom(a)} {op} (guint32) {atom(b)}'
    raise ValueError(f'unsupported condition {mn}')


def negate(c):
    for op in ('==', '!=', '>=', '<=', '>', '<'):
        parts = c.split(f' {op} ')
        if len(parts) == 2 and '&&' not in c and '||' not in c:
            return f'{parts[0]} {NEG[op]} {parts[1]}'
    return f'!({c})'


def lift(va, args, fold, track=False):
    pe = Pe(str(DLL))
    insns = {}
    for ins in pe.md.disasm(pe.img[va - pe.base:va - pe.base + 0x10000], va):
        insns[ins.address] = ins
        if ins.mnemonic == 'int3':
            break
    live = liveness(pe, insns)
    # explore the state graph: node = (addr, live state key)
    nodes = {}   # id -> dict(kind, stmt/cond, succ)
    ids = {}
    work = [(va, Machine(pe, args, fold, track))]
    order = []

    def node_id(a, m):
        k = (a, m.key(live.get(a)))
        if k not in ids:
            ids[k] = len(ids)
            work.append((a, m))
        return ids[k]
    ids[(va, work[0][1].key(live.get(va)))] = 0
    first = work.pop()
    work.append(first)
    while work:
        a, m = work.pop()
        nid = ids[(a, m.key(live.get(a)))]
        if nid in nodes:
            continue
        stmts = []
        start_addr = a
        while True:
            ins = insns[a]
            mn, ops = ins.mnemonic, ins.operands
            nxt = a + ins.size
            if mn == 'ret':
                nodes[nid] = {'kind': 'ret', 'stmts': stmts, 'addr': start_addr,
                              'final': tuple((k, v if is_c(v) else txt(v)) for k, v in sorted(m.mem.items()))}
                break
            if mn in ('push',):
                m.reg['rsp'] = add(m.reg['rsp'], 8, neg=True)
                m.stack[m.reg['rsp'][2]] = ('saved', ins.op_str)
            elif mn == 'pop':
                m.reg['rsp'] = add(m.reg['rsp'], 8)
            elif mn in ('mov', 'movsxd', 'movzx', 'movsx') and ops[0].type == x86.X86_OP_MEM:
                dst = m.addr(ops[0])
                v = m.val(ops[1])
                if dst[1] == 'STACK':
                    m.stack[dst[2]] = v
                elif dst[1].startswith('FLAG:'):
                    if m.track:
                        m.mem[dst[1][5:]] = v
                    else:
                        stmts.append(f'*{dst[1][5:]} = {txt(v)};')
                else:
                    raise ValueError(f'store to {dst} at {a:#x}')
            elif mn in ('mov', 'movsxd', 'movzx', 'movsx'):
                v = m.val(ops[1])
                if isinstance(v, tuple) and v[0] == 'saved':
                    v = ('saved', v[1])
                m.setreg(ops[0], v)
            elif mn == 'lea':
                m.setreg(ops[0], m.addr(ops[1]))
            elif mn in ('add', 'sub'):
                r = add(m.val(ops[0]), m.val(ops[1]), neg=(mn == 'sub'))
                if ops[0].type == x86.X86_OP_REG and R64[pe.md.reg_name(ops[0].reg)] == 'rsp':
                    m.reg['rsp'] = r
                else:
                    m.setreg(ops[0], r)
                    m.flags = ('res', r, 0)
            elif mn == 'imul' and len(ops) == 3:
                m.setreg(ops[0], mul(m.val(ops[1]), s32(ops[2].imm)))
            elif mn == 'xor' and ops[0].reg == ops[1].reg:
                m.setreg(ops[0], 0)
                m.flags = ('res', 0, 0)
            elif mn == 'cmp':
                m.flags = ('cmp', m.val(ops[0]), m.val(ops[1]))
            elif mn == 'test':
                m.flags = ('test', m.val(ops[0]), m.val(ops[1]))
            elif mn == 'bt':
                m.flags = ('bt', m.val(ops[0]), m.val(ops[1]))
            elif mn == 'jmp':
                nxt = ops[0].imm
            elif mn.startswith('j'):
                c = cond_of(m.flags, mn)
                if is_c(c):
                    nxt = ops[0].imm if c else nxt
                else:
                    t = node_id(ops[0].imm, m.clone())
                    f = node_id(nxt, m.clone())
                    nodes[nid] = {'kind': 'br', 'cond': c, 'succ': (t, f), 'stmts': stmts, 'addr': start_addr}
                    break
            elif mn in ('nop',):
                pass
            else:
                raise ValueError(f'unsupported {a:#x}: {mn} {ins.op_str}')
            a = nxt
            # blocks start at jump targets (possible join points)
            if is_target(a, insns):
                nodes[nid] = {'kind': 'go', 'succ': (node_id(a, m.clone()),), 'stmts': stmts, 'addr': start_addr}
                break
    return nodes


_targets = None


def liveness(pe, insns):
    """live 64-bit registers at each instruction (flags excluded), backward dataflow."""
    info = {}
    for a, ins in insns.items():
        if ins.mnemonic == 'int3':
            continue
        rd, wr = ins.regs_access()
        use = {R64.get(pe.md.reg_name(r)) for r in rd} - {None}
        dfn = {R64.get(pe.md.reg_name(r)) for r in wr} - {None}
        if ins.mnemonic == 'xor' and ins.operands[0].type == x86.X86_OP_REG and \
                ins.operands[1].type == x86.X86_OP_REG and ins.operands[0].reg == ins.operands[1].reg:
            use = set()
        succ = []
        mn = ins.mnemonic
        if mn == 'ret':
            use |= {'rax'}
        elif mn == 'jmp':
            succ = [ins.operands[0].imm]
        elif mn.startswith('j'):
            succ = [ins.operands[0].imm, a + ins.size]
        else:
            succ = [a + ins.size]
        info[a] = (use, dfn, [x for x in succ if x in insns])
    live = {a: set() for a in info}
    changed = True
    while changed:
        changed = False
        for a in sorted(info, reverse=True):
            use, dfn, succ = info[a]
            out = set()
            for x in succ:
                out |= live.get(x, set())
            new = use | (out - dfn)
            if new != live[a]:
                live[a] = new
                changed = True
    for a in live:
        live[a] |= {'rsp'}
    return live


def is_target(a, insns):
    global _targets
    if _targets is None:
        _targets = set()
        for ins in insns.values():
            if ins.mnemonic.startswith('j') and ins.operands[0].type == x86.X86_OP_IMM:
                _targets.add(ins.operands[0].imm)
    return a in _targets


# ---- structuring -------------------------------------------------------------------------------
def postdom(nodes):
    exit_ = -1
    succ = {n: list(d.get('succ', ())) or [exit_] for n, d in nodes.items()}
    # reverse topological order (DAG)
    order, seen = [], set()

    def dfs(n):
        if n in seen or n == exit_:
            return
        seen.add(n)
        for s in succ[n]:
            dfs(s)
        order.append(n)
    sys.setrecursionlimit(100000)
    dfs(0)
    # ipdom via intersection of post-dominator chains (Cooper et al. on the reverse graph)
    rpo_index = {n: i for i, n in enumerate(order)}   # successors come first
    rpo_index[exit_] = -1
    ipdom = {exit_: exit_}

    def intersect(a, b):
        while a != b:
            while rpo_index[a] < rpo_index[b]:
                b = ipdom[b]
            while rpo_index[b] < rpo_index[a]:
                a = ipdom[a]
        return a
    for n in order:
        ss = succ[n]
        p = ss[0]
        for s in ss[1:]:
            p = intersect(p, s)
        ipdom[n] = p
    return ipdom


# boolean conditions: ('a', text) atoms, ('and', (..)), ('or', (..)), ('not', x)
def c_not(c):
    if c[0] == 'a':
        return ('a', negate(c[1]))
    if c[0] == 'not':
        return c[1]
    if c[0] == 'and':
        return ('or', tuple(c_not(x) for x in c[1]))
    return ('and', tuple(c_not(x) for x in c[1]))


def c_join(kind, a, b):
    items = []
    for x in (a, b):
        items.extend(x[1] if x[0] == kind else (x,))
    return (kind, tuple(items))


def c_txt(c, ctx='top'):
    if c[0] == 'a':
        return c[1]
    if c[0] == 'not':
        return f'!({c_txt(c[1])})'
    op = ' && ' if c[0] == 'and' else ' || '
    body = op.join(c_txt(x, c[0]) for x in c[1])
    return body if ctx in ('top', c[0]) or (ctx == 'or' and c[0] == 'and') else f'({body})'


class Formula:
    """hash-consed if-then-else DAG: TRUE, FALSE, or ite(cond, A, B)."""
    def __init__(self):
        self.table = {}
        self.TRUE, self.FALSE = ('T',), ('F',)

    def ite(self, c, a, b):
        if a is b:
            return a
        # ite(c, ite(c2, A, B), B) -> ite(c && c2, A, B)
        if a[0] == 'ite' and a[3] is b:
            return self.ite(c_join('and', c, a[1]), a[2], b)
        # ite(c, A, ite(c2, A, B)) -> ite(c || c2, A, B)
        if b[0] == 'ite' and b[2] is a:
            return self.ite(c_join('or', c, b[1]), a, b[3])
        key = (repr(c), id(a), id(b))
        if key not in self.table:
            self.table[key] = ('ite', c, a, b)
        return self.table[key]

    def cond(self, f, names=None):
        """f as a condition AST; nodes in `names` print as their name."""
        if f[0] != 'ite':
            raise ValueError('constant formula')
        _, c, a, b = f
        sub = (lambda x: ('a', names[id(x)]) if names is not None and id(x) in names
               else self.cond(x, names))
        if a is self.TRUE and b is self.FALSE:
            return c
        if a is self.FALSE and b is self.TRUE:
            return c_not(c)
        if b is self.FALSE:
            return c_join('and', c, sub(a))
        if a is self.TRUE:
            return c_join('or', c, sub(b))
        if a is self.FALSE:
            return c_join('and', c_not(c), sub(b))
        if b is self.TRUE:
            return c_join('or', c_not(c), sub(a))
        return c_join('or', c_join('and', c, sub(a)), c_join('and', c_not(c), sub(b)))

    def size(self, f, memo=None):
        """number of atoms when printed as a tree"""
        memo = {} if memo is None else memo
        if f[0] != 'ite':
            return 0
        if id(f) not in memo:
            memo[id(f)] = 1 + self.size(f[2], memo) + self.size(f[3], memo)
        return memo[id(f)]

    def shared(self, root, prefix):
        """(definitions, root condition): ite nodes used more than once become named booleans."""
        refs, order, seen = {}, [], set()

        def walk(f):
            if f[0] != 'ite':
                return
            for x in (f[2], f[3]):
                if x[0] == 'ite':
                    refs[id(x)] = refs.get(id(x), 0) + 1
            if id(f) in seen:
                return
            seen.add(id(f))
            walk(f[2])
            walk(f[3])
            order.append(f)
        walk(root)
        names, defs = {}, []
        for f in order:   # children first
            if refs.get(id(f), 0) > 1 and f is not root:
                defs.append((f'{prefix}{len(names)}', self.cond(f, names)))
                names[id(f)] = defs[-1][0]
        return defs, self.cond(root, names)


def region(nodes, ipdom, n, stop):
    seen, st = set(), [n]
    while st:
        x = st.pop()
        if x in seen or x == stop or x == -1:
            continue
        seen.add(x)
        st.extend(nodes[x].get('succ', ()))
    return seen


WARN = []
SHARED = [0]


def split_effect_tails(nodes):
    """give every predecessor its own copy of a shared `store; jump` block (tail-merged by the
    compiler), so each rule owns its effect."""
    preds = {}
    for n, d in nodes.items():
        for z in d.get('succ', ()):
            preds.setdefault(z, []).append(n)
    nxt = max(nodes) + 1
    for n in list(nodes):
        d = nodes[n]
        if not d['stmts'] or d['kind'] != 'go' or len(preds.get(n, ())) < 2:
            continue
        for p in preds[n][1:]:
            c = dict(d)
            nodes[nxt] = c
            pd = nodes[p]
            pd['succ'] = tuple(nxt if z == n else z for z in pd['succ'])
            nxt += 1


def ipdom_sub(succ, root):
    """immediate post-dominator of root in the DAG `succ` (dict node -> successors); terminals
    (no successors) all flow into a virtual exit (None)."""
    order, seen = [], set()

    def dfs(v):
        if v in seen:
            return
        seen.add(v)
        for z in succ.get(v, ()):
            dfs(z)
        order.append(v)
    dfs(root)
    idx = {v: i for i, v in enumerate(order)}
    idx[None] = -1
    ip = {None: None}

    def inter(a, b):
        while a != b:
            while idx[a] < idx[b]:
                b = ip[b]
            while idx[b] < idx[a]:
                a = ip[a]
        return a
    for v in order:
        ss = list(succ.get(v, ())) or [None]
        p = ss[0]
        for z in ss[1:]:
            p = inter(p, z)
        ip[v] = p
    return ip


def rule_at(nodes, n, stop):
    """find (x_group, C, inside): the first effect x reachable from n through pure decisions, the
    continuation C where the paths that miss x meet, and the decision nodes of the rule."""
    # pure nodes reachable from n and the effects at their frontier, by BFS distance
    pure, dist, frontier, q = set(), {n: 0}, [], [n]
    while q:
        y = q.pop(0)
        d = nodes[y]
        if y != n and (y == stop or y == -1 or d['stmts'] or d['kind'] == 'ret'):
            if y not in (stop, -1):
                frontier.append(y)
            continue
        pure.add(y)
        for z in d['succ']:
            if z not in dist:
                dist[z] = dist[y] + 1
                q.append(z)
    tried = set()
    cands = []
    for x in sorted(frontier, key=lambda y: dist[y]):
        key = (nodes[x]['addr'], tuple(nodes[x]['stmts']), nodes[x].get('succ'))
        if key in tried:
            continue
        tried.add(key)
        copies = sorted({y for y in frontier if (nodes[y]['addr'], tuple(nodes[y]['stmts']),
                                                  nodes[y].get('succ')) == key}, key=lambda y: dist[y])
        for k in range(1, len(copies) + 1):
            cands.append(set(copies[:k]))
    for group in cands:
        # graph without edges into the group; nodes that lead only into it drop out
        succ = {}
        for y in pure:
            succ[y] = [z for z in nodes[y]['succ'] if z not in group]
        changed = True
        while changed:
            changed = False
            for y in list(succ):
                if y != n and not succ[y] and all(z in group or z not in succ for z in nodes[y]['succ']) \
                        and any(z in group for z in nodes[y]['succ']):
                    del succ[y]
                    changed = True
            for y in succ:
                k = [z for z in succ[y] if z in succ or z not in pure]
                if k != succ[y]:
                    succ[y] = k
                    changed = True
        # can reach the group through pure nodes?
        reach_g = set()
        rp = {}
        for y in pure:
            for z in nodes[y]['succ']:
                rp.setdefault(z, []).append(y)
        st = list(group)
        while st:
            y = st.pop()
            for p in rp.get(y, ()):
                if p not in reach_g:
                    reach_g.add(p)
                    st.append(p)
        if n not in succ:
            c = stop
        else:
            ipm = ipdom_sub(succ, n)
            c = ipm[n]
            while c is not None and c in reach_g:   # the rule continues past c
                c = ipm[c]
            if c is None:
                continue
        # decision nodes of the rule: pure nodes on n -> group paths that avoid c
        preds = {}
        for y in pure:
            for z in nodes[y]['succ']:
                preds.setdefault(z, []).append(y)
        inside, st = set(), list(group)
        while st:
            y = st.pop()
            for p in preds.get(y, ()):
                if p not in inside and p != c:
                    inside.add(p)
                    st.append(p)
        if n not in inside:
            continue
        exits = {z for y in inside for z in nodes[y]['succ'] if z not in inside and z not in group}
        if exits <= {c}:
            return group, c, inside
    return None


def group_formula(nodes, n, group, inside, F, memo):
    if n in group:
        return F.TRUE
    if n not in inside:
        return F.FALSE
    if n in memo:
        return memo[n]
    d = nodes[n]
    if d['kind'] == 'go':
        r = group_formula(nodes, d['succ'][0], group, inside, F, memo)
    else:
        r = F.ite(('a', d['cond']), group_formula(nodes, d['succ'][0], group, inside, F, memo),
                  group_formula(nodes, d['succ'][1], group, inside, F, memo))
    memo[n] = r
    return r


def reaches(nodes, a, b, stop):
    return b in region(nodes, None, a, stop) or a == b


def structure(nodes, ipdom, n, stop):
    out = []
    while n != stop and n != -1:
        d = nodes[n]
        out.extend(('S', s) for s in d['stmts'])
        if d['kind'] == 'ret':
            break
        if d['kind'] == 'go':
            n = d['succ'][0]
            continue
        j = ipdom[n]
        reg = region(nodes, ipdom, n, j)
        if not any(nodes[x]['stmts'] or nodes[x]['kind'] == 'ret' for x in reg):
            n = j
            continue
        r = rule_at(nodes, n, j)
        if r is not None:
            group, c, inside = r
            x = min(group)
            F = Formula()
            f = group_formula(nodes, n, group, inside, F, {})
            if F.size(f) > 40:   # large DAG: name its shared parts (evaluated here, in order)
                SHARED[0] += 1
                defs, cond = F.shared(f, f'b{SHARED[0]}_')
                out.extend(('S', f'const gboolean {nm} = {c_txt(cc)};') for nm, cc in defs)
            else:
                cond = F.cond(f)
            if c != j and reaches(nodes, x, c, j):
                out.append(('IF', cond, structure(nodes, ipdom, x, c), []))
                n = c
            else:
                out.append(('IF', cond, structure(nodes, ipdom, x, j), structure(nodes, ipdom, c, j)))
                n = j
            continue
        WARN.append(f'split at {d["addr"]:#x}')
        t = structure(nodes, ipdom, d['succ'][0], j)
        f = structure(nodes, ipdom, d['succ'][1], j)
        out.append(('IF', ('a', d['cond']), t, f))
        n = j
    return out


def simplify(block):
    res = []
    for it in block:
        if it[0] != 'IF':
            res.append(it)
            continue
        _, c, t, f = it
        t, f = simplify(t), simplify(f)
        if not t and not f:
            continue
        if not t:
            c, t, f = c_not(c), f, []
        while not f and len(t) == 1 and t[0][0] == 'IF' and not t[0][3]:
            c, t = c_join('and', c, t[0][1]), t[0][2]
        res.append(('IF', c, t, f))
    return res


def emit(block, ind=1):
    lines = []
    sp = '  ' * ind
    for it in block:
        if it[0] == 'S':
            lines.append(sp + it[1])
        else:
            _, c, t, f = it
            lines.append(f'{sp}if ({c_txt(c)})')
            if len(t) == 1 and t[0][0] == 'S':
                lines.append(sp + '  ' + t[0][1])
            else:
                lines.append(sp + '  {')
                lines.extend(emit(t, ind + 2))
                lines.append(sp + '  }')
            if f and len(f) == 1 and f[0][0] == 'IF':
                sub = emit(f, ind)
                lines.append(sp + 'else ' + sub[0].lstrip())
                lines.extend(sub[1:])
            elif f:
                lines.append(sp + 'else')
                lines.append(sp + '  {')
                lines.extend(emit(f, ind + 2))
                lines.append(sp + '  }')
    return lines


def main():
    va = int(sys.argv[1], 16)
    name = sys.argv[2]
    final_mode = '--final' in sys.argv
    rest = [a for a in sys.argv[3:] if not a.startswith('--') and '=' not in a]
    fold = {}
    for a in sys.argv[3:]:
        if '=' in a:
            k, v = a.split('=')
            fold[int(k, 16)] = int(v, 0)
    flag_args = rest[1:] if rest and rest[0] == 'params' else rest
    nodes = lift(va, flag_args, fold, track=final_mode)
    body = []
    if final_mode:
        # one condition per final flag value (conditions over the inputs and <flag>_in)
        groups = {}
        for n, d in nodes.items():
            if d['kind'] == 'ret':
                groups.setdefault(d['final'], []).append(n)
        initial = tuple((k, f'{k}_in') for k in sorted(flag_args))
        F = Formula()
        for final, rets in sorted(groups.items(), key=lambda g: repr(g[0])):
            if final == initial:
                continue
            targets, memo = set(rets), {}

            def rf(n):
                if n in memo:
                    return memo[n]
                d = nodes[n]
                if d['kind'] == 'ret':
                    r = F.TRUE if n in targets else F.FALSE
                elif d['kind'] == 'go':
                    r = rf(d['succ'][0])
                else:
                    r = F.ite(('a', d['cond']), rf(d['succ'][0]), rf(d['succ'][1]))
                memo[n] = r
                return r
            f = rf(0)
            stmts = [('S', f'*{k} = {v};') for (k, v), (_, v0) in zip(final, initial) if v != v0]
            if f is F.TRUE:
                body.extend(stmts)
            elif f is not F.FALSE:
                defs, root = F.shared(f, f'r{len(body)}_')
                body.extend(('S', f'const gboolean {n} = {c_txt(c)};') for n, c in defs)
                body.append(('IF', root, stmts, []))
    else:
        split_effect_tails(nodes)
        ipdom = postdom(nodes)
        body = simplify(structure(nodes, ipdom, 0, -1))
    text = '\n'.join(emit(body))
    params = sorted({w for w in text.replace('(', ' ').replace(')', ' ').split() if w.startswith('params_')} |
                    {w for t in TEMPS for w in t.replace('(', ' ').replace(')', ' ').split() if w.startswith('params_')})
    sig = ['const GoodixChicagoMatchScoreRecord *rec'] + [f'gint32 {p}' for p in params] + \
          [f'gint32 *{f}' for f in flag_args]
    print(f'/* generated by tools/algo/lift_rules.py from AlgoChicago {va:#x}'
          f' (folded params: {", ".join(f"+{k:#x}={v}" for k, v in fold.items())}) */')
    print(f'static void\n{name} ({", ".join(sig)})\n{{')
    for t, n in TEMPS.items():
        if n in text.replace('(', ' ').replace(')', ' ').split():
            print(f'  const gint32 {n} = {t};')
    for fl in flag_args:
        if f'{fl}_in' in text:
            print(f'  const gint32 {fl}_in = *{fl};')
    print(text)
    print('}')
    print(f'/* nodes: {len(nodes)}; splits: {len(WARN)} */', file=sys.stderr)
    for w in WARN[:20]:
        print(w, file=sys.stderr)


if __name__ == '__main__':
    main()
