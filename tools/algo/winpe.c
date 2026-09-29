// Minimal in-process loader for a self-contained x64 Windows DLL on Linux.
// Purpose: run Goodix AlgoMilan.dll (fingerprint algorithm) natively so its
// enroll/identify quality can be measured, and later called from the driver.
//
// It maps the image at its preferred base, applies base relocations if needed,
// resolves the ~118 imported api-ms-win-* / kernel32 functions to small
// Win64-ABI (ms_abi) stubs implemented here, runs the CRT DllMain, and exposes
// GetProcAddress over the export table.
//
// This is not a Windows emulator: only the happy path of a compute library is
// supported. File/registry/console calls fail cleanly; the C++ exception
// machinery is stubbed and must not actually be triggered.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "winpe.h"

#define MS __attribute__((ms_abi))
#define DLL_PROCESS_ATTACH 1

static int verbose;
void winpe_set_verbose(int v) { verbose = v; }

// ---- PE structures (only the fields we use) ----
#pragma pack(push, 1)
struct dos { uint16_t magic; uint8_t pad[58]; int32_t lfanew; };
struct data_dir { uint32_t rva, size; };
struct opt64 {
  uint16_t magic; uint8_t major, minor; uint32_t sizecode, sizeinit, sizeuninit;
  uint32_t entry, basecode; uint64_t imagebase;
  uint32_t secalign, filealign; uint16_t v[6]; uint32_t win32ver, sizeimage,
      sizehdr, checksum; uint16_t subsys, dllchar; uint64_t stackres, stackcom,
      heapres, heapcom; uint32_t loaderflags, numdd; struct data_dir dd[16];
};
struct nt { uint32_t sig; struct { uint16_t machine, nsec; uint32_t ts, symtab,
      nsym; uint16_t optsize, chars; } fh; struct opt64 oh; };
struct sec { char name[8]; uint32_t vsize, vaddr, rawsize, rawptr; uint8_t
      pad[12]; uint32_t chars; };
struct imp_desc { uint32_t olt, ts, fwd, name, iat; };
struct base_reloc { uint32_t va, size; };
struct exp_dir { uint32_t flags, ts; uint16_t major, minor; uint32_t name,
      base, nfunc, nname, afunc, aname, aord; };
#pragma pack(pop)

#define DD_EXPORT 0
#define DD_IMPORT 1
#define DD_BASERELOC 5

static uint8_t *g_base;

// ---- error/last-error + a tiny heap that tracks sizes ----
static uint32_t g_lasterr;
static MS uint32_t k_GetLastError(void) { return g_lasterr; }
static MS void k_SetLastError(uint32_t e) { g_lasterr = e; }

static MS void *k_GetProcessHeap(void) { return (void *)1; }
static MS void *k_HeapAlloc(void *h, uint32_t flags, uint64_t size) {
  (void)h;
  if (size > UINT64_MAX - 16) return NULL;
  uint64_t *p = malloc(size + 16);
  if (!p) return NULL;
  p[0] = size;
  if (flags & 8) memset(p + 2, 0, size);        // HEAP_ZERO_MEMORY
  return p + 2;
}
static MS void *k_HeapReAlloc(void *h, uint32_t flags, void *ptr, uint64_t sz) {
  (void)h;
  if (!ptr) return k_HeapAlloc(h, flags, sz);
  if (sz > UINT64_MAX - 16) return NULL;
  uint64_t *p = (uint64_t *)ptr - 2;
  uint64_t old = p[0];
  uint64_t *n = realloc(p, sz + 16);
  if (!n) return NULL;
  n[0] = sz;
  // HEAP_ZERO_MEMORY: the grown part reads as zero, as on Windows
  if ((flags & 8) && sz > old) memset((uint8_t *)(n + 2) + old, 0, sz - old);
  return n + 2;
}
static MS int k_HeapFree(void *h, uint32_t flags, void *ptr) {
  (void)h; (void)flags;
  if (ptr) free((uint64_t *)ptr - 2);
  return 1;
}
static MS uint64_t k_HeapSize(void *h, uint32_t flags, void *ptr) {
  (void)h; (void)flags;
  return ptr ? *((uint64_t *)ptr - 2) : (uint64_t)-1;
}

// ---- process/thread/debug ----
static MS void *k_GetCurrentProcess(void) { return (void *)(intptr_t)-1; }
static MS uint32_t k_GetCurrentProcessId(void) { return 1000; }
static MS uint32_t k_GetCurrentThreadId(void) { return 1001; }
static MS int k_IsDebuggerPresent(void) { return 0; }
static MS int k_IsProcessorFeaturePresent(uint32_t f) { (void)f; return 1; }
static MS void *k_EncodePointer(void *p) { return p; }
static MS void *k_DecodePointer(void *p) { return p; }

// ---- TLS ----
static void *g_tls[256];
static uint32_t g_tls_next = 1;
#define TLS_OUT_OF_INDEXES 0xffffffffu
static MS uint32_t k_TlsAlloc(void) {
  return g_tls_next < 256 ? g_tls_next++ : TLS_OUT_OF_INDEXES;
}
static MS int k_TlsFree(uint32_t i) { (void)i; return 1; }
static MS void *k_TlsGetValue(uint32_t i) { return i < 256 ? g_tls[i] : NULL; }
static MS int k_TlsSetValue(uint32_t i, void *v) {
  if (i < 256) g_tls[i] = v;
  return 1;
}

// ---- fiber-local storage (newer CRTs): same single-thread table as TLS ----
static MS uint32_t k_FlsAlloc(void *cb) { (void)cb; return k_TlsAlloc(); }

// ---- critical sections / slist (single threaded => no-ops) ----
static MS int k_InitializeCriticalSectionAndSpinCount(void *c, uint32_t s) {
  (void)c; (void)s; return 1;
}
static MS int k_InitializeCriticalSectionEx(void *c, uint32_t s, uint32_t f) {
  (void)c; (void)s; (void)f; return 1;
}
static MS void k_CriticalSectionNop(void *c) { (void)c; }

// ---- kernel objects: fake handles, nothing ever runs concurrently ----
// AlgoChicago's CRT/log code creates an event + worker thread at attach; the
// thread is never started (the algorithm itself is synchronous).
static MS void *k_FakeHandle(void) { return (void *)(intptr_t)0x1234; }
static MS int k_True(void) { return 1; }
static MS uint32_t k_WaitObject0(void) { return 0; }   // WAIT_OBJECT_0
static MS void *k_InvalidHandle(void) { return (void *)(intptr_t)-1; }
static MS void k_Sleep(uint32_t ms) { (void)ms; }
static MS void k_GetLocalTime(uint16_t *st) {   // SYSTEMTIME
  time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
  st[0] = tm.tm_year + 1900; st[1] = tm.tm_mon + 1; st[2] = tm.tm_wday;
  st[3] = tm.tm_mday; st[4] = tm.tm_hour; st[5] = tm.tm_min; st[6] = tm.tm_sec; st[7] = 0;
}
static MS void k_InitializeSListHead(void *h) { if (h) memset(h, 0, 16); }

// ---- time ----
static MS void k_GetSystemTimeAsFileTime(uint64_t *ft) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  if (ft) *ft = (uint64_t)(ts.tv_sec + 11644473600ULL) * 10000000ULL +
                ts.tv_nsec / 100;
}
static MS int k_QueryPerformanceCounter(uint64_t *c) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  if (c) *c = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
  return 1;
}
static MS int k_QueryPerformanceFrequency(uint64_t *f) {
  if (f) *f = 1000000000ULL;
  return 1;
}

// ---- module/proc resolution ----
static void *pe_getproc(const char *name);
static MS void *k_GetModuleHandleW(void *name) {
  return name ? NULL : g_base;
}
static MS int k_GetModuleHandleExW(uint32_t flags, void *name, void **out) {
  (void)flags; (void)name;
  if (out) *out = g_base;
  return 1;
}
static MS void *k_GetProcAddress(void *mod, const char *name) {
  (void)mod;
  if ((uintptr_t)name < 0x10000) return NULL;   // by ordinal, unsupported
  return pe_getproc(name);
}
static MS void *k_LoadLibraryExW(void *name, void *h, uint32_t f) {
  (void)name; (void)h; (void)f; return NULL;
}

// ---- code-page / string conversion (ASCII 1:1 is enough) ----
static MS uint32_t k_GetACP(void) { return 1252; }
static MS uint32_t k_GetOEMCP(void) { return 437; }
static MS int k_IsValidCodePage(uint32_t cp) { (void)cp; return 1; }
static MS int k_GetCPInfo(uint32_t cp, void *info) {
  (void)cp;
  if (info) { memset(info, 0, 24); *(uint32_t *)info = 1; }  // MaxCharSize=1
  return 1;
}
static MS int k_MultiByteToWideChar(uint32_t cp, uint32_t f, const char *s,
                                    int slen, uint16_t *w, int wlen) {
  (void)cp; (void)f;
  int n = slen < 0 ? (int)strlen(s) + 1 : slen;
  if (wlen == 0) return n;
  int m = n < wlen ? n : wlen;
  for (int i = 0; i < m; i++) w[i] = (uint8_t)s[i];
  return m;
}
static MS int k_WideCharToMultiByte(uint32_t cp, uint32_t f, const uint16_t *w,
                                    int wlen, char *s, int slen, void *a,
                                    void *b) {
  (void)cp; (void)f; (void)a; (void)b;
  int n = wlen < 0 ? 0 : wlen;
  if (wlen < 0) { while (w[n]) n++; n++; }
  if (slen == 0) return n;
  int m = n < slen ? n : slen;
  for (int i = 0; i < m; i++) s[i] = (char)(w[i] & 0xff);
  return m;
}
static MS int k_LCMapStringW(uint32_t l, uint32_t f, const uint16_t *src,
                             int slen, uint16_t *dst, int dlen) {
  (void)l; (void)f;
  int n = slen < 0 ? 0 : slen;
  if (slen < 0) { while (src[n]) n++; n++; }
  if (dlen == 0) return n;
  int m = n < dlen ? n : dlen;
  for (int i = 0; i < m; i++) dst[i] = src[i];
  return m;
}
static MS uint32_t k_GetUserDefaultLCID(void) { return 0x409; }

// ---- console/env (used by CRT startup) ----
static uint16_t g_env[2] = {0, 0};
static MS void *k_GetStdHandle(uint32_t n) { return (void *)(intptr_t)n; }
static MS void *k_GetCommandLineW(void) {
  static uint16_t c[4] = {'x', 0}; return c;
}
static MS void *k_GetCommandLineA(void) { static char c[4] = "x"; return c; }
static MS void *k_GetEnvironmentStringsW(void) { return g_env; }
static MS int k_FreeEnvironmentStringsW(void *p) { (void)p; return 1; }
static MS void k_GetStartupInfoW(void *si) { if (si) memset(si, 0, 104); }
static MS int k_WriteConsoleW(void *h, const void *b, uint32_t n, void *w,
                              void *r) {
  (void)h; (void)b; (void)r;
  if (w) *(uint32_t *)w = n;
  return 1;
}

// ---- eventing (ETW): success no-ops ----
static MS uint32_t k_EventRegister(void *a, void *b, void *c, void *d) {
  (void)a; (void)b; (void)c; (void)d; return 0;
}

// ---- fatal ----
static MS void k_ExitProcess(uint32_t c) {
  fprintf(stderr, "[winpe] ExitProcess(%u)\n", c); exit(c);
}
static MS void k_RaiseException(uint32_t code, uint32_t fl, uint32_t n,
                                void *args) {
  (void)fl; (void)n; (void)args;
  fprintf(stderr, "[winpe] RaiseException(0x%x) — unsupported\n", code);
  abort();
}

static MS void k_OutputDebugStringA(const char *s) {
  if (s) fprintf(stderr, "[dll] %s", s);
}
static MS void k_OutputDebugStringW(const uint16_t *s) {
  if (!s) return;
  fprintf(stderr, "[dll] ");
  for (; *s; s++) fputc(*s < 128 ? (int)*s : '?', stderr);
}

// Generic stub: returns 0. Named so verbose mode can report first use.
struct named_stub { const char *name; };
static MS uint64_t generic_stub(void) { return 0; }

// A per-name logging stub is overkill; we track misses via the resolver.
struct import_impl { const char *name; void *fn; };
static const struct import_impl impls[] = {
  {"GetLastError", k_GetLastError}, {"SetLastError", k_SetLastError},
  {"GetProcessHeap", k_GetProcessHeap}, {"HeapAlloc", k_HeapAlloc},
  {"HeapReAlloc", k_HeapReAlloc}, {"HeapFree", k_HeapFree},
  {"HeapSize", k_HeapSize}, {"GetCurrentProcess", k_GetCurrentProcess},
  {"GetCurrentProcessId", k_GetCurrentProcessId},
  {"GetCurrentThreadId", k_GetCurrentThreadId},
  {"IsDebuggerPresent", k_IsDebuggerPresent},
  {"IsProcessorFeaturePresent", k_IsProcessorFeaturePresent},
  {"EncodePointer", k_EncodePointer}, {"DecodePointer", k_DecodePointer},
  {"TlsAlloc", k_TlsAlloc}, {"TlsFree", k_TlsFree},
  {"TlsGetValue", k_TlsGetValue}, {"TlsSetValue", k_TlsSetValue},
  {"InitializeCriticalSectionAndSpinCount",
   k_InitializeCriticalSectionAndSpinCount},
  {"InitializeSListHead", k_InitializeSListHead},
  {"FlsAlloc", k_FlsAlloc}, {"FlsFree", k_TlsFree},
  {"FlsGetValue", k_TlsGetValue}, {"FlsSetValue", k_TlsSetValue},
  {"InitializeCriticalSectionEx", k_InitializeCriticalSectionEx},
  {"InitializeCriticalSection", k_CriticalSectionNop},
  {"EnterCriticalSection", k_CriticalSectionNop},
  {"LeaveCriticalSection", k_CriticalSectionNop},
  {"DeleteCriticalSection", k_CriticalSectionNop},
  {"CreateEventW", k_FakeHandle}, {"CreateThread", k_FakeHandle},
  {"SetEvent", k_True}, {"CloseHandle", k_True},
  {"WaitForSingleObject", k_WaitObject0},
  {"WaitForMultipleObjects", k_WaitObject0},
  {"CreateFileW", k_InvalidHandle}, {"FindFirstFileW", k_InvalidHandle},
  {"Sleep", k_Sleep}, {"GetLocalTime", k_GetLocalTime},
  {"GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime},
  {"QueryPerformanceCounter", k_QueryPerformanceCounter},
  {"QueryPerformanceFrequency", k_QueryPerformanceFrequency},
  {"GetModuleHandleW", k_GetModuleHandleW},
  {"GetModuleHandleExW", k_GetModuleHandleExW},
  {"GetProcAddress", k_GetProcAddress}, {"LoadLibraryExW", k_LoadLibraryExW},
  {"GetACP", k_GetACP}, {"GetOEMCP", k_GetOEMCP},
  {"IsValidCodePage", k_IsValidCodePage}, {"GetCPInfo", k_GetCPInfo},
  {"MultiByteToWideChar", k_MultiByteToWideChar},
  {"WideCharToMultiByte", k_WideCharToMultiByte},
  {"LCMapStringW", k_LCMapStringW},
  {"GetUserDefaultLCID", k_GetUserDefaultLCID},
  {"GetStdHandle", k_GetStdHandle}, {"GetCommandLineW", k_GetCommandLineW},
  {"GetCommandLineA", k_GetCommandLineA},
  {"GetEnvironmentStringsW", k_GetEnvironmentStringsW},
  {"FreeEnvironmentStringsW", k_FreeEnvironmentStringsW},
  {"GetStartupInfoW", k_GetStartupInfoW}, {"WriteConsoleW", k_WriteConsoleW},
  {"OutputDebugStringA", k_OutputDebugStringA},
  {"OutputDebugStringW", k_OutputDebugStringW},
  {"EventRegister", k_EventRegister}, {"ExitProcess", k_ExitProcess},
  {"TerminateProcess", k_ExitProcess}, {"RaiseException", k_RaiseException},
  {NULL, NULL},
};

static void *resolve_import(const char *name) {
  for (const struct import_impl *p = impls; p->name; p++)
    if (!strcmp(p->name, name)) return p->fn;
  if (verbose) fprintf(stderr, "[winpe] stub: %s -> 0\n", name);
  return (void *)generic_stub;
}

// ---- export table lookup ----
static struct nt *g_nt;
static void *pe_getproc(const char *want) {
  struct data_dir ed = g_nt->oh.dd[DD_EXPORT];
  if (!ed.rva) return NULL;
  struct exp_dir *e = (void *)(g_base + ed.rva);
  uint32_t *names = (void *)(g_base + e->aname);
  uint16_t *ords = (void *)(g_base + e->aord);
  uint32_t *funcs = (void *)(g_base + e->afunc);
  for (uint32_t i = 0; i < e->nname; i++) {
    const char *nm = (const char *)(g_base + names[i]);
    if (!strcmp(nm, want)) return g_base + funcs[ords[i]];
  }
  return NULL;
}

void *winpe_getproc(const char *name) { return pe_getproc(name); }

// ---- loader ----
// Windows x64 code reaches the TEB through the gs segment (stack checks read
// gs:[0x10] StackLimit, thread-locals use gs:[0x58]). glibc keeps its own TLS
// in fs, so we can point gs at a synthetic TEB. StackLimit is set low so the
// __chkstk fast path is always taken against our real Linux stack.
#ifndef ARCH_SET_GS
#define ARCH_SET_GS 0x1001
#endif
static void setup_teb(void) {
  size_t n = 0x4000;
  uint8_t *teb = mmap(NULL, n, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uint8_t *tls = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memset(teb, 0, n);
  memset(tls, 0, 0x2000);
  *(uint64_t *)(teb + 0x08) = 0x7ffffffff000ULL;  // NT_TIB.StackBase
  *(uint64_t *)(teb + 0x10) = 0x1000ULL;          // NT_TIB.StackLimit
  *(uint64_t *)(teb + 0x30) = (uint64_t)teb;       // NT_TIB.Self
  *(uint64_t *)(teb + 0x58) = (uint64_t)tls;       // ThreadLocalStoragePointer
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb) != 0)
    perror("arch_prctl SET_GS");
}

void *winpe_load(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror("open dll"); return NULL; }
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *file = malloc(sz);
  if (fread(file, 1, sz, f) != (size_t)sz) { fclose(f); return NULL; }
  fclose(f);

  struct dos *d = (void *)file;
  int32_t lfanew = d->lfanew;
  struct nt *nt = (void *)(file + lfanew);
  uint64_t want = nt->oh.imagebase;
  size_t image = nt->oh.sizeimage;

  void *mapped = mmap((void *)want, image, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapped == MAP_FAILED) {
    mapped = mmap(NULL, image, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) { perror("mmap"); free(file); return NULL; }
  }
  g_base = mapped;
  int64_t delta = (int64_t)((uint64_t)g_base - want);

  memcpy(g_base, file, nt->oh.sizehdr);
  struct sec *sec = (void *)((uint8_t *)&nt->oh + nt->fh.optsize);
  for (int i = 0; i < nt->fh.nsec; i++)
    if (sec[i].rawsize)
      memcpy(g_base + sec[i].vaddr, file + sec[i].rawptr, sec[i].rawsize);
  free(file);
  g_nt = (void *)(g_base + lfanew);

  // base relocations
  if (delta) {
    struct data_dir rd = g_nt->oh.dd[DD_BASERELOC];
    uint8_t *p = g_base + rd.rva, *end = p + rd.size;
    while (p < end) {
      struct base_reloc *b = (void *)p;
      uint16_t *ent = (void *)(p + sizeof(*b));
      int n = (b->size - sizeof(*b)) / 2;
      for (int i = 0; i < n; i++) {
        if ((ent[i] >> 12) == 10)   // IMAGE_REL_BASED_DIR64
          *(uint64_t *)(g_base + b->va + (ent[i] & 0xfff)) += delta;
      }
      p += b->size;
    }
  }

  // imports
  struct data_dir id = g_nt->oh.dd[DD_IMPORT];
  for (struct imp_desc *im = (void *)(g_base + id.rva); im->name; im++) {
    uint64_t *oft = im->olt ? (void *)(g_base + im->olt)
                            : (void *)(g_base + im->iat);
    uint64_t *iat = (void *)(g_base + im->iat);
    for (int i = 0; oft[i]; i++) {
      const char *nm = (const char *)(g_base + oft[i] + 2);  // skip hint
      iat[i] = (uint64_t)resolve_import(nm);
    }
  }

  setup_teb();
  // run CRT DllMain(hinst, DLL_PROCESS_ATTACH, NULL)
  typedef int MS (*dllmain_t)(void *, uint32_t, void *);
  dllmain_t entry = (dllmain_t)(g_base + g_nt->oh.entry);
  int r = entry(g_base, DLL_PROCESS_ATTACH, NULL);
  if (verbose) fprintf(stderr, "[winpe] DllMain returned %d\n", r);
  if (!r) { fprintf(stderr, "[winpe] DllMain failed\n"); return NULL; }
  return g_base;
}

void *winpe_base(void) { return g_base; }

// Redirect the function at preferred-base VA `va` to `fn` (an ms_abi function)
// by overwriting its first 12 bytes with `movabs rax, fn; jmp rax`. Used to
// route AlgoMilan's internal logger into our own printf for diagnostics.
int winpe_hook(uint64_t va, void *fn) {
  if (!g_base || !g_nt) return -1;
  uint8_t *p = g_base + (va - g_nt->oh.imagebase);
  p[0] = 0x48; p[1] = 0xb8;                 // movabs rax, imm64
  memcpy(p + 2, &fn, 8);
  p[10] = 0xff; p[11] = 0xe0;               // jmp rax
  return 0;
}
