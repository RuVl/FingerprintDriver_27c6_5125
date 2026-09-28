# gdb Python trace of the flag re-evaluation 0x180019800 inside the AlgoChicago type-24 identify
# scheduler (call site 0x180029921), docs/stage5-match.md.
#   cd tools/algo && ALGO=chicago TRACE_OUT=/tmp/reeval.txt gdb --batch -x dll_reeval_trace.py --args ./algo_eval4 12
# Output per re-evaluated record:
#   "PROBE <rec>"  (identifyImage), then
#   " RE idx=<gallery idx> in s/c=<status>/<conf> 258c0 s/c 23340 s/c 19a30 s/c 1fdd0 s/c out s/c"
# where each stage shows (status, conf) after that step (a skipped step repeats the previous state),
# plus the 26 record dwords.
import gdb, struct, os
out = open(os.environ.get("TRACE_OUT", "/dev/stdout"), "w")
inf = None
def rd(addr):
    return struct.unpack("<i", bytes(inf.read_memory(addr, 4)))[0]
def reg(r):
    return int(gdb.parse_and_eval("$" + r)) & 0xffffffffffffffff
class BP(gdb.Breakpoint):
    def __init__(self, addr, fn):
        super().__init__("*0x%x" % addr, internal=True)
        self.fn = fn
    def stop(self):
        self.fn()
        return False
dsts = None
cur = {}
def on_identify():
    out.write("PROBE %d\n" % ((reg("rcx") - dsts) // 64))
def on_geom():   # 0x180029449: gallery index of the record being scored
    cur["idx"] = rd(reg("rsp") + 0x74)
def on_entry():  # 0x180019800(rec, q, c, params, &conf, &status)
    rsp = reg("rsp")
    cur["rec"] = reg("rcx")
    cur["conf"] = struct.unpack("<Q", bytes(inf.read_memory(rsp + 0x28, 8)))[0]
    cur["status"] = struct.unpack("<Q", bytes(inf.read_memory(rsp + 0x30, 8)))[0]
    cur["log"] = ["in %d/%d" % (rd(cur["status"]), rd(cur["conf"]))]
def mark(tag):
    def f():
        cur["log"].append("%s %d/%d" % (tag, rd(cur["status"]), rd(cur["conf"])))
    return f
def on_exit():   # 0x180029926
    rec = cur["rec"]
    vals = [rd(rec + 4 * k) for k in range(26)]
    out.write(" RE idx=%d %s out %d/%d\n  rec %s\n" % (cur.get("idx", -1), " ".join(cur["log"]),
              rd(cur["status"]), rd(cur["conf"]), " ".join(str(v) for v in vals)))
    out.flush()
gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("break winpe_load")
gdb.execute("run")
gdb.execute("finish")
inf = gdb.selected_inferior()
dsts = int(gdb.parse_and_eval("(long)&dsts"))
BP(0x18000d170, on_identify)
BP(0x180029449, on_geom)
BP(0x180019800, on_entry)
BP(0x1800198cf, mark("258c0"))   # join points: state after each (possibly skipped) step
BP(0x18001990e, mark("23340"))
BP(0x180019953, mark("19a30"))
BP(0x1800199a4, mark("1fdd0"))
BP(0x180029926, on_exit)

gdb.execute("continue")
out.close()
