# gdb Python trace of the AlgoChicago type-24 identify scheduler (0x180028e70), per probe/record.
#   cd tools/algo && ALGO=chicago VERBOSE=1 TRACE_OUT=/tmp/dll_trace.txt \
#     gdb --batch -x dll_match_trace.py --args ./algo_eval4 12
# Output: "PROBE <rec>", probe packed_resolution/quality/coverage, per gallery record
# " R idx g0 g4 g8 r28" (after geometry passes, 0x180029449), "rec"/"rec2" = 26 dwords of the
# 0x68-byte score record before the prefilter / after the study filter, status/conf, ACCEPT,
# post-loop path, "SEL i" (best-geometry status record -> result+0x648, the selected
# subtemplate) and the final ratio.  Compare with MTRACE=1 MFIXES=1 ./port_eval.sh.
import gdb, struct, os
out = open(os.environ.get("TRACE_OUT", "/dev/stdout"), "w")
inf = None
def rd(addr, n=4):
    return struct.unpack("<i", bytes(inf.read_memory(addr, 4)))[0]
def reg(r):
    return int(gdb.parse_and_eval("$" + r)) & 0xffffffffffffffff
def i32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v
class BP(gdb.Breakpoint):
    def __init__(self, addr, fn):
        super().__init__("*0x%x" % addr, internal=True)
        self.fn = fn
    def stop(self):
        self.fn()
        return False
dsts = None
def on_identify():
    out.write("PROBE %d\n" % ((reg("rcx") - dsts) // 64))
def on_matcher():
    p = reg("rdx")
    out.write(" probe res=0x%x q=%d c=%d n=%d\n" % (rd(p + 0x140) & 0xffffffff, rd(p + 0x10c), rd(p + 0x110), rd(p + 0xf0)))
def on_geom():
    rsp, rbx = reg("rsp"), reg("rbx")
    out.write(" R idx=%d g0=%d g4=%d g8=%d r28=%d\n" % (rd(rsp + 0x74), rd(rbx), rd(rbx + 4), rd(rbx + 8), rd(rbx + 0x28)))
def on_xfchk():
    e = i32(reg("rax"))
    if e: out.write("  xfbad %d\n" % e)
def on_prefilter():
    e = i32(reg("rax"))
    rbx = reg("rbx")
    vals = [rd(rbx + 4 * k) for k in range(26)]
    out.write("  rec " + " ".join(str(v) for v in vals) + "\n")
    if e: out.write("  prefilter %d\n" % e)
def on_late():
    e = i32(reg("rax"))
    rbp = reg("rbp")
    out.write("  late rc=%d rejcnt=%d\n" % (e, rd(rbp - 0x78)))
def on_study():
    rsp = reg("rsp"); rbx = reg("rbx")
    vals = [rd(rbx + 4 * k) for k in range(26)]
    out.write("  status=%d conf=%d rec2 %s\n" % (rd(rsp + 0x70), rd(rsp + 0x60), " ".join(str(v) for v in vals)))
def on_accept():
    out.write("  ACCEPT\n")
def on_sel():
    out.write("  SEL %d\n" % i32(reg("r15")))   # 0x180029e9c: result+0x648 = selected subtemplate
def on_post():
    out.write(" POST count=%d\n" % rd(reg("rsp") + 0x78))
def on_fb27():
    out.write(" best27fb0=%d\n" % i32(reg("rax")))
def on_fallback():
    out.write(" fallback rc=%d ratio=%d\n" % (i32(reg("rax")), rd(reg("r15"))))
def on_ratio():
    out.write(" RATIO %d\n" % i32(reg("rcx")))
    out.flush()
def on_winpe():
    pass
gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("break winpe_load")
gdb.execute("run")
gdb.execute("finish")
inf = gdb.selected_inferior()
dsts = int(gdb.parse_and_eval("(long)&dsts"))
BP(0x18000d170, on_identify)
BP(0x180028740, on_matcher)
BP(0x180029449, on_geom)
BP(0x180029494, on_xfchk)
BP(0x1800296c7, on_prefilter)
BP(0x180029a31, on_late)
BP(0x180029a94, on_study)
BP(0x180029c2a, on_accept)
BP(0x180029e9c, on_sel)
BP(0x18002a3b3, on_post)
BP(0x18002a3d9, on_fb27)
BP(0x18002a48d, on_fallback)
BP(0x18000d400, on_ratio)
gdb.execute("continue")
out.close()
