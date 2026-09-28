// Replacement for AlgoMilan's internal logger 0x1800088e0 (printf-like, ms_abi
// varargs). The DLL formats "Algo:<msg>" and sends it to a WPP tracer we don't
// emulate; hooking it gives us the algorithm's own diagnostics (matcher ratio,
// feature counts, template state) on stderr.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "algolog.h"

static int (*g_filter)(const char *line);
static FILE *g_out;
void algolog_set(FILE *out, int (*filter)(const char *)) {
  g_out = out; g_filter = filter;
}

static __attribute__((ms_abi)) void algo_log(const char *fmt, ...) {
  if (!g_out) return;
  __builtin_ms_va_list ap;
  __builtin_ms_va_start(ap, fmt);
  char line[1024]; size_t n = 0;
  for (const char *p = fmt; *p && n < sizeof line - 64;) {
    if (*p != '%') { line[n++] = *p++; continue; }
    // copy one conversion spec: %[flags][width][.prec][length]conv
    char spec[32]; size_t k = 0;
    spec[k++] = *p++;
    while (*p && strchr("-+ #0123456789.*", *p) && k < 20) spec[k++] = *p++;
    int longs = 0;
    while (*p && strchr("hlLzjtI", *p) && k < 26) {
      if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't') longs++;
      if (*p == 'I') { // MS %I64d
        p++; if (p[0] == '6' && p[1] == '4') p += 2; longs = 2; continue;
      }
      p++;   // drop length modifiers, we widen explicitly below
    }
    char conv = *p ? *p++ : 0;
    spec[k] = 0;
    char buf[256];
    switch (conv) {
    case '%': buf[0] = '%'; buf[1] = 0; break;
    case 'd': case 'i': {
      int64_t v = __builtin_va_arg(ap, int64_t);
      if (longs < 2) v = (int32_t)v;
      strcat(spec, "lld"); snprintf(buf, sizeof buf, spec, (long long)v); break;
    }
    case 'u': case 'x': case 'X': case 'o': case 'c': {
      uint64_t v = __builtin_va_arg(ap, uint64_t);
      if (longs < 2 && conv != 'c') v = (uint32_t)v;
      if (conv == 'c') { strcat(spec, "c"); snprintf(buf, sizeof buf, spec, (int)v); }
      else { size_t L = strlen(spec); spec[L] = 'l'; spec[L + 1] = 'l';
             spec[L + 2] = conv; spec[L + 3] = 0;
             snprintf(buf, sizeof buf, spec, (unsigned long long)v); }
      break;
    }
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
      double v = __builtin_va_arg(ap, double);
      size_t L = strlen(spec); spec[L] = conv; spec[L + 1] = 0;
      snprintf(buf, sizeof buf, spec, v); break;
    }
    case 's': {
      const char *s = __builtin_va_arg(ap, const char *);
      snprintf(buf, sizeof buf, "%s", s ? s : "(null)"); break;
    }
    case 'p': snprintf(buf, sizeof buf, "%p", __builtin_va_arg(ap, void *)); break;
    default: snprintf(buf, sizeof buf, "%s%c", spec, conv); break;
    }
    size_t bl = strlen(buf);
    if (n + bl >= sizeof line - 2) break;
    memcpy(line + n, buf, bl); n += bl;
  }
  __builtin_ms_va_end(ap);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  line[n] = 0;
  if (g_filter && !g_filter(line)) return;
  fprintf(g_out, "[algo] %s\n", line);
}

void *algolog_fn(void) { return (void *)algo_log; }
