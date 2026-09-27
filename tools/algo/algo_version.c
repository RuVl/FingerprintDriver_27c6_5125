#include <stdio.h>
#include "winpe.h"
#define MS __attribute__((ms_abi))
typedef int MS (*ver_t)(char *);
int main(int argc, char **argv) {
  winpe_set_verbose(1);
  void *base = winpe_load(argc > 1 ? argv[1] : "../../win-driver/AlgoMilan.dll");
  if (!base) { fprintf(stderr, "load failed\n"); return 1; }
  printf("loaded at %p\n", base);
  const char *syms[] = {"getAlgorithmVersionWrapper", "getAlgorithmVersion"};
  for (int i = 0; i < 2; i++) {
    void *fn = winpe_getproc(syms[i]);
    printf("%s = %p\n", syms[i], fn);
    if (fn) {
      char buf[256] = {0};
      int r = ((ver_t)fn)(buf);
      printf("  -> ret %d, version '%s'\n", r, buf);
    }
  }
  return 0;
}
